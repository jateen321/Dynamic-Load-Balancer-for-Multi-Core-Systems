#include "config.h"
#include "logger.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

LoadBalancerConfig* init_default_config(void) {
    LoadBalancerConfig* config = malloc(sizeof(LoadBalancerConfig));
    if (!config) return NULL;

    config->max_tasks = 10;
    config->monitoring_interval_ms = 100;
    config->high_load_threshold = 80.0;
    config->low_load_threshold = 20.0;
    config->load_history_size = 10;
    config->enable_load_prediction = 1;
    config->enable_detailed_logging = 1;
    config->log_file_path = strdup("./cpu_balancer.log");
    /* Default to every online core; main() overrides from argv. */
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    config->num_cpus = (online > 0) ? (int)online : 1;
    config->scheduling_policy = SCHED_PREDICTIVE;
    config->enable_work_stealing = 1;
    config->on_task_complete = NULL;
    config->on_task_complete_user_data = NULL;

    if (!config->log_file_path) {
        free(config);
        return NULL;
    }

    return config;
}

const char* scheduling_policy_name(SchedulingPolicy policy) {
    switch (policy) {
        case SCHED_ROUND_ROBIN: return "Round Robin";
        case SCHED_LEAST_LOAD:  return "Least Load";
        case SCHED_PREDICTIVE:  return "Predictive";
        default:                return "Unknown";
    }
}

/* ------------------------------------------------------------------------
 * Minimal hand-rolled JSON reader for the flat, one-level config object.
 *
 * The project deliberately links nothing beyond pthread/m/rt, so a real JSON
 * library is off the table for a format this simple: a single object, known
 * keys, scalar values only. Everything below exists to parse exactly that
 * safely against untrusted local input, not to be a general JSON parser.
 * ------------------------------------------------------------------------ */

/* A config file has no business being large; capping the read means a
 * mis-pointed path (a device file, a multi-GB log someone renamed) can't
 * make us allocate unbounded memory before we've even looked at the bytes. */
#define CONFIG_MAX_FILE_SIZE (1 << 20) /* 1 MiB */
/* Bounds a single decoded string value (only log_file_path uses this today,
 * but the cap protects any future string field the same way). */
#define CONFIG_MAX_STRING_LEN (1 << 16) /* 64 KiB */

typedef struct {
    const char* data;
    size_t len;
    size_t pos;
    int error;
    char error_msg[128];
} JsonParser;

typedef enum { JVAL_STRING, JVAL_NUMBER, JVAL_BOOL, JVAL_NULL, JVAL_OTHER } JsonValueType;

typedef struct {
    JsonValueType type;
    char* str;       /* owned; only meaningful when type == JVAL_STRING */
    double num;      /* only meaningful when type == JVAL_NUMBER */
    int has_frac;    /* number literal had a '.' or exponent */
    int boolean;     /* only meaningful when type == JVAL_BOOL */
} JsonValue;

static void json_value_free(JsonValue* v) {
    if (v && v->type == JVAL_STRING) {
        free(v->str);
        v->str = NULL;
    }
}

/* Records only the first error: once something has gone wrong, later
 * "expected X, got garbage" noise from unwinding is rarely more useful than
 * the original complaint, and only the first message is ever logged. */
static void json_set_error(JsonParser* p, const char* msg) {
    if (!p->error) {
        p->error = 1;
        snprintf(p->error_msg, sizeof(p->error_msg), "%s", msg);
    }
}

static void json_skip_ws(JsonParser* p) {
    while (p->pos < p->len) {
        char c = p->data[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->pos++;
        } else {
            break;
        }
    }
}

/* Skips whitespace and returns the next byte without consuming it, or -1 at
 * end of input. Every parsing function below relies on this having already
 * skipped whitespace, so a matched character can just be consumed with
 * p->pos++ immediately after. */
static int json_peek(JsonParser* p) {
    json_skip_ws(p);
    if (p->pos >= p->len) return -1;
    return (unsigned char)p->data[p->pos];
}

static int json_match_literal(JsonParser* p, const char* lit) {
    size_t l = strlen(lit);
    if (p->pos + l > p->len) return 0;
    if (strncmp(p->data + p->pos, lit, l) != 0) return 0;
    p->pos += l;
    return 1;
}

/* Parses a JSON string starting at the opening quote (already confirmed
 * present by the caller via json_peek). Returns a freshly malloc'd,
 * NUL-terminated, unescaped copy, or NULL with p->error set. */
static char* json_parse_string(JsonParser* p) {
    if (json_peek(p) != '"') {
        json_set_error(p, "expected a string");
        return NULL;
    }
    p->pos++; /* consume opening quote */

    size_t cap = 64, out_len = 0;
    char* out = malloc(cap);
    if (!out) {
        json_set_error(p, "out of memory");
        return NULL;
    }

    for (;;) {
        if (p->pos >= p->len) {
            json_set_error(p, "unterminated string");
            free(out);
            return NULL;
        }
        char c = p->data[p->pos++];
        if (c == '"') break;
        if ((unsigned char)c < 0x20) {
            json_set_error(p, "control character in string");
            free(out);
            return NULL;
        }

        char decoded;
        if (c == '\\') {
            if (p->pos >= p->len) {
                json_set_error(p, "unterminated escape sequence");
                free(out);
                return NULL;
            }
            char esc = p->data[p->pos++];
            switch (esc) {
                case '"':  decoded = '"';  break;
                case '\\': decoded = '\\'; break;
                case '/':  decoded = '/';  break;
                case 'b':  decoded = '\b'; break;
                case 'f':  decoded = '\f'; break;
                case 'n':  decoded = '\n'; break;
                case 'r':  decoded = '\r'; break;
                case 't':  decoded = '\t'; break;
                case 'u': {
                    /* Config values don't need full UTF-16 surrogate-pair
                     * support; decode the 4 hex digits enough to stay valid
                     * and emit '?' for anything outside plain ASCII. */
                    if (p->pos + 4 > p->len) {
                        json_set_error(p, "truncated \\u escape");
                        free(out);
                        return NULL;
                    }
                    unsigned int cp = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = p->data[p->pos++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9')      cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        else {
                            json_set_error(p, "invalid \\u escape");
                            free(out);
                            return NULL;
                        }
                    }
                    decoded = (cp >= 0x20 && cp < 0x7f) ? (char)cp : '?';
                    break;
                }
                default:
                    json_set_error(p, "invalid escape sequence");
                    free(out);
                    return NULL;
            }
        } else {
            decoded = c;
        }

        if (out_len + 1 >= cap) {
            if (cap >= CONFIG_MAX_STRING_LEN) {
                json_set_error(p, "string value too long");
                free(out);
                return NULL;
            }
            size_t new_cap = cap * 2;
            char* grown = realloc(out, new_cap);
            if (!grown) {
                json_set_error(p, "out of memory");
                free(out);
                return NULL;
            }
            out = grown;
            cap = new_cap;
        }
        out[out_len++] = decoded;
    }

    out[out_len] = '\0';
    return out;
}

/* Parses a JSON number token per the grammar (optional '-', int part, optional
 * frac, optional exponent). has_frac tells the caller whether the literal
 * looked non-integral, purely as a hint for the 0/1-as-bool convenience --
 * the value itself is always returned as a double. */
static int json_parse_number(JsonParser* p, double* out_val, int* out_has_frac) {
    json_skip_ws(p);
    size_t start = p->pos;

    if (p->pos < p->len && p->data[p->pos] == '-') p->pos++;

    if (p->pos >= p->len || !isdigit((unsigned char)p->data[p->pos])) {
        json_set_error(p, "invalid number");
        return 0;
    }
    if (p->data[p->pos] == '0') {
        p->pos++;
    } else {
        while (p->pos < p->len && isdigit((unsigned char)p->data[p->pos])) p->pos++;
    }

    int has_frac = 0;
    if (p->pos < p->len && p->data[p->pos] == '.') {
        has_frac = 1;
        p->pos++;
        if (p->pos >= p->len || !isdigit((unsigned char)p->data[p->pos])) {
            json_set_error(p, "invalid number");
            return 0;
        }
        while (p->pos < p->len && isdigit((unsigned char)p->data[p->pos])) p->pos++;
    }
    if (p->pos < p->len && (p->data[p->pos] == 'e' || p->data[p->pos] == 'E')) {
        has_frac = 1;
        p->pos++;
        if (p->pos < p->len && (p->data[p->pos] == '+' || p->data[p->pos] == '-')) p->pos++;
        if (p->pos >= p->len || !isdigit((unsigned char)p->data[p->pos])) {
            json_set_error(p, "invalid number");
            return 0;
        }
        while (p->pos < p->len && isdigit((unsigned char)p->data[p->pos])) p->pos++;
    }

    size_t tok_len = p->pos - start;
    /* A file full of digits is adversarial input, not a real config value;
     * truncate the copy so strtod always sees a valid, in-bounds token
     * instead of us reading past a fixed buffer. The (wrong) magnitude that
     * results is harmless -- callers clamp before ever casting to int. */
    char tokbuf[64];
    if (tok_len >= sizeof(tokbuf)) tok_len = sizeof(tokbuf) - 1;
    memcpy(tokbuf, p->data + start, tok_len);
    tokbuf[tok_len] = '\0';

    char* endptr = NULL;
    double val = strtod(tokbuf, &endptr);
    if (endptr == tokbuf) {
        json_set_error(p, "invalid number");
        return 0;
    }

    *out_val = val;
    if (out_has_frac) *out_has_frac = has_frac;
    return 1;
}

/* Skips a balanced {...} or [...] the caller doesn't care about (an unknown
 * key's value, or a known key given a container instead of a scalar).
 * Deliberately loose about matching '{' with '}' and '[' with ']'
 * specifically -- any open increments depth and any close decrements it --
 * because this is a skip, not a validator; strict bracket matching adds
 * complexity this format never needs. Iterative, so unlike a recursive
 * descent it can't stack-overflow on adversarially deep nesting. */
static int json_skip_container(JsonParser* p) {
    p->pos++; /* consume the opening bracket the caller peeked */
    int depth = 1;
    while (depth > 0) {
        if (p->pos >= p->len) {
            json_set_error(p, "unterminated object or array");
            return 0;
        }
        char ch = p->data[p->pos];
        if (ch == '"') {
            char* s = json_parse_string(p); /* consumes through the closing quote */
            if (!s) return 0;
            free(s);
            continue;
        }
        if (ch == '{' || ch == '[') depth++;
        else if (ch == '}' || ch == ']') depth--;
        p->pos++;
    }
    return 1;
}

static int json_parse_value(JsonParser* p, JsonValue* out) {
    memset(out, 0, sizeof(*out));
    int c = json_peek(p);

    if (c == '"') {
        char* s = json_parse_string(p);
        if (!s) return 0;
        out->type = JVAL_STRING;
        out->str = s;
        return 1;
    } else if (c == 't') {
        if (!json_match_literal(p, "true")) { json_set_error(p, "invalid literal"); return 0; }
        out->type = JVAL_BOOL;
        out->boolean = 1;
        return 1;
    } else if (c == 'f') {
        if (!json_match_literal(p, "false")) { json_set_error(p, "invalid literal"); return 0; }
        out->type = JVAL_BOOL;
        out->boolean = 0;
        return 1;
    } else if (c == 'n') {
        if (!json_match_literal(p, "null")) { json_set_error(p, "invalid literal"); return 0; }
        out->type = JVAL_NULL;
        return 1;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        double val;
        int has_frac;
        if (!json_parse_number(p, &val, &has_frac)) return 0;
        out->type = JVAL_NUMBER;
        out->num = val;
        out->has_frac = has_frac;
        return 1;
    } else if (c == '{' || c == '[') {
        if (!json_skip_container(p)) return 0;
        out->type = JVAL_OTHER;
        return 1;
    } else {
        json_set_error(p, "unexpected character");
        return 0;
    }
}

static void warn_type_mismatch(const char* config_path, const char* key, const char* expected) {
    log_message(LOG_WARNING,
                "load_config: '%s' has key \"%s\" whose value is not %s; using the default for that field",
                config_path, key, expected);
}

/* Clamp before casting: strtod() on a bogus/huge literal can hand back
 * +-infinity, and converting an out-of-range double to int is undefined
 * behaviour in C, not just a wraparound. */
static int json_number_to_int(double d) {
    if (d > (double)INT_MAX) return INT_MAX;
    if (d < (double)INT_MIN) return INT_MIN;
    return (int)d;
}

/* true/false are the normal case; a bare 0/1 is accepted too since it's a
 * common, unambiguous shorthand in hand-written config files. Only a whole
 * 0 or 1 counts -- 0.5 or 2 are still a type mismatch. */
static int apply_bool_field(int* field, const JsonValue* val) {
    if (val->type == JVAL_BOOL) {
        *field = val->boolean ? 1 : 0;
        return 1;
    }
    if (val->type == JVAL_NUMBER && !val->has_frac && (val->num == 0.0 || val->num == 1.0)) {
        *field = (val->num == 1.0) ? 1 : 0;
        return 1;
    }
    return 0;
}

static int parse_scheduling_policy_name(const char* s, SchedulingPolicy* out) {
    if (strcasecmp(s, "round_robin") == 0) { *out = SCHED_ROUND_ROBIN; return 1; }
    if (strcasecmp(s, "least_load") == 0)  { *out = SCHED_LEAST_LOAD;  return 1; }
    if (strcasecmp(s, "predictive") == 0)  { *out = SCHED_PREDICTIVE;  return 1; }
    return 0;
}

/* Applies one already-parsed key/value pair onto config, which starts out
 * holding init_default_config()'s values. A key whose value has the wrong
 * type is logged and left at whatever it already holds (the default, unless
 * an earlier occurrence of the same key in a malformed-but-not-rejected file
 * set it first) rather than aborting the parse. Unknown keys are silently
 * ignored for forward compatibility with newer config files. */
static void apply_config_field(LoadBalancerConfig* config, const char* key,
                                const JsonValue* val, const char* config_path) {
    if (strcmp(key, "max_tasks") == 0) {
        if (val->type == JVAL_NUMBER) config->max_tasks = json_number_to_int(val->num);
        else warn_type_mismatch(config_path, key, "a number");
    } else if (strcmp(key, "monitoring_interval_ms") == 0) {
        if (val->type == JVAL_NUMBER) config->monitoring_interval_ms = json_number_to_int(val->num);
        else warn_type_mismatch(config_path, key, "a number");
    } else if (strcmp(key, "high_load_threshold") == 0) {
        if (val->type == JVAL_NUMBER) config->high_load_threshold = val->num;
        else warn_type_mismatch(config_path, key, "a number");
    } else if (strcmp(key, "low_load_threshold") == 0) {
        if (val->type == JVAL_NUMBER) config->low_load_threshold = val->num;
        else warn_type_mismatch(config_path, key, "a number");
    } else if (strcmp(key, "load_history_size") == 0) {
        if (val->type == JVAL_NUMBER) config->load_history_size = json_number_to_int(val->num);
        else warn_type_mismatch(config_path, key, "a number");
    } else if (strcmp(key, "enable_load_prediction") == 0) {
        if (!apply_bool_field(&config->enable_load_prediction, val))
            warn_type_mismatch(config_path, key, "a boolean");
    } else if (strcmp(key, "enable_detailed_logging") == 0) {
        if (!apply_bool_field(&config->enable_detailed_logging, val))
            warn_type_mismatch(config_path, key, "a boolean");
    } else if (strcmp(key, "enable_work_stealing") == 0) {
        if (!apply_bool_field(&config->enable_work_stealing, val))
            warn_type_mismatch(config_path, key, "a boolean");
    } else if (strcmp(key, "log_file_path") == 0) {
        if (val->type == JVAL_STRING) {
            char* dup = strdup(val->str);
            if (dup) {
                free(config->log_file_path);
                config->log_file_path = dup;
            }
            /* strdup() OOM: keep the existing (default) path rather than
             * losing it, and don't treat allocator exhaustion as a config
             * error worth a warning of its own. */
        } else {
            warn_type_mismatch(config_path, key, "a string");
        }
    } else if (strcmp(key, "scheduling_policy") == 0) {
        SchedulingPolicy policy;
        if (val->type == JVAL_STRING && parse_scheduling_policy_name(val->str, &policy)) {
            config->scheduling_policy = policy;
        } else {
            log_message(LOG_WARNING,
                        "load_config: '%s' has key \"scheduling_policy\" with an unrecognized value "
                        "(expected \"round_robin\", \"least_load\", or \"predictive\"); using the default",
                        config_path);
        }
    } else if (strcmp(key, "num_cpus") == 0) {
        /* Deliberately not file-configurable: main() always overrides this
         * from argv after loading the config, so a value here would only be
         * misleading. Accept the key (for files shared with tooling that
         * does write it) but ignore it rather than rejecting the file. */
    }
    /* Any other unknown key: ignore silently. */
}

typedef enum {
    CONFIG_READ_OK,
    CONFIG_READ_CANNOT_OPEN,
    CONFIG_READ_TOO_LARGE,
    CONFIG_READ_IO_ERROR,
    CONFIG_READ_OOM
} ConfigReadResult;

static ConfigReadResult read_config_file(const char* path, char** out_buf, size_t* out_len) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return CONFIG_READ_CANNOT_OPEN;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return CONFIG_READ_IO_ERROR; }
    long size = ftell(fp);
    if (size < 0) { fclose(fp); return CONFIG_READ_IO_ERROR; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return CONFIG_READ_IO_ERROR; }

    if (size > CONFIG_MAX_FILE_SIZE) { fclose(fp); return CONFIG_READ_TOO_LARGE; }

    char* buf = malloc((size_t)size + 1);
    if (!buf) { fclose(fp); return CONFIG_READ_OOM; }

    /* Short reads (e.g. the file shrank between ftell and fread) are fine:
     * we trust what fread actually returned, not what ftell predicted. */
    size_t n = fread(buf, 1, (size_t)size, fp);
    fclose(fp);

    buf[n] = '\0';
    *out_buf = buf;
    *out_len = n;
    return CONFIG_READ_OK;
}

/* Parses the top-level "{ "key": value, ... }" object, applying each field
 * onto config as it goes. Returns 0 with p->error set on any syntax problem;
 * the caller discards config entirely on failure rather than keeping a
 * partially-applied result, so nothing here needs to be transactional. */
static int parse_config_object(JsonParser* p, LoadBalancerConfig* config, const char* config_path) {
    if (json_peek(p) != '{') {
        json_set_error(p, "expected '{' at start of file");
        return 0;
    }
    p->pos++;

    if (json_peek(p) == '}') {
        p->pos++;
    } else {
        for (;;) {
            if (json_peek(p) != '"') {
                json_set_error(p, "expected a string key");
                return 0;
            }
            char* key = json_parse_string(p);
            if (!key) return 0;

            if (json_peek(p) != ':') {
                json_set_error(p, "expected ':' after key");
                free(key);
                return 0;
            }
            p->pos++;

            JsonValue val;
            if (!json_parse_value(p, &val)) {
                free(key);
                return 0;
            }

            apply_config_field(config, key, &val, config_path);

            json_value_free(&val);
            free(key);

            int c = json_peek(p);
            if (c == ',') {
                p->pos++;
                continue;
            }
            if (c == '}') {
                p->pos++;
                break;
            }
            json_set_error(p, "expected ',' or '}' after value");
            return 0;
        }
    }

    /* Trailing content after the object (stray text, a second top-level
     * value) is treated as malformed rather than silently ignored -- it
     * almost certainly means the file isn't what the author intended. */
    if (json_peek(p) != -1) {
        json_set_error(p, "unexpected data after top-level object");
        return 0;
    }

    return 1;
}

LoadBalancerConfig* load_config(const char* config_path) {
    if (!config_path) {
        log_message(LOG_WARNING, "load_config: no config path given; using default configuration");
        return init_default_config();
    }

    char* buf = NULL;
    size_t buf_len = 0;
    ConfigReadResult rr = read_config_file(config_path, &buf, &buf_len);
    if (rr != CONFIG_READ_OK) {
        const char* why;
        switch (rr) {
            case CONFIG_READ_CANNOT_OPEN: why = "could not open the file"; break;
            case CONFIG_READ_TOO_LARGE:   why = "file exceeds the 1 MiB config size limit"; break;
            case CONFIG_READ_OOM:         why = "out of memory while reading the file"; break;
            case CONFIG_READ_IO_ERROR:    why = "I/O error while reading the file"; break;
            default:                      why = "unknown error"; break;
        }
        log_message(LOG_WARNING, "load_config: '%s' -- %s; using default configuration", config_path, why);
        return init_default_config();
    }

    LoadBalancerConfig* config = init_default_config();
    if (!config) {
        free(buf);
        return NULL;
    }

    JsonParser parser;
    parser.data = buf;
    parser.len = buf_len;
    parser.pos = 0;
    parser.error = 0;
    parser.error_msg[0] = '\0';

    if (!parse_config_object(&parser, config, config_path)) {
        /* Syntax error: the spec here is "don't attempt partial recovery",
         * so discard whatever fields got applied before the error and hand
         * back a clean set of defaults instead of a half-parsed config. */
        log_message(LOG_WARNING,
                    "load_config: '%s' is not valid JSON (%s, near byte %zu of %zu); using default configuration",
                    config_path,
                    parser.error_msg[0] ? parser.error_msg : "syntax error",
                    parser.pos, buf_len);
        free_config(config);
        free(buf);
        return init_default_config();
    }

    free(buf);
    return config;
}

void free_config(LoadBalancerConfig* config) {
    if (config) {
        free(config->log_file_path);
        free(config);
    }
}
