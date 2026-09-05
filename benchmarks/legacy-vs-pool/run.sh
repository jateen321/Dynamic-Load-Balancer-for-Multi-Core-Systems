#!/usr/bin/env bash
# Reproduces the worker-pool-vs-thread-per-task comparison documented in
# benchmarks/legacy-vs-pool/RESULTS.md.
#
# The "before" design (thread-per-task, one pthread_create per task) only
# exists in git history — it was replaced in commit 32cb34e. This script
# checks that commit's parent (3c73725) out into a throwaway git worktree,
# compiles old_bench.c directly against those sources (bypassing its old
# CMakeLists.txt, which builds a single `cpu_balancer` binary from main.c —
# a standalone driver is simpler than teaching that old build system about a
# second entry point), then builds new_bench.c against the CURRENT tree's
# cpu_balancer_core static library and runs both with identical workloads.
#
# Usage: benchmarks/legacy-vs-pool/run.sh [num_cores] [task_counts...]
#   num_cores:   defaults to nproc
#   task_counts: defaults to "50 200 800 2000"
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LEGACY_COMMIT="3c73725"   # last commit before the worker-pool rewrite (32cb34e)
WORK_DIR="$(mktemp -d)"
trap 'git -C "$REPO_ROOT" worktree remove --force "$WORK_DIR/old" >/dev/null 2>&1 || true; rm -rf "$WORK_DIR"' EXIT

NUM_CORES="${1:-$(nproc)}"
shift || true
TASK_COUNTS=("${@:-50 200 800 2000}")
if [ "${#TASK_COUNTS[@]}" -eq 1 ] && [[ "${TASK_COUNTS[0]}" == *" "* ]]; then
    # allow "50 200 800 2000" as one quoted arg too
    read -r -a TASK_COUNTS <<< "${TASK_COUNTS[0]}"
fi

MIN_MS=5
MAX_MS=15
SEED=42

echo "== Setting up the pre-rewrite (thread-per-task) worktree at $LEGACY_COMMIT ==" >&2
git -C "$REPO_ROOT" worktree add --detach "$WORK_DIR/old" "$LEGACY_COMMIT" >&2

echo "== Building old_bench (thread-per-task) ==" >&2
gcc -std=c11 -Wall -Wextra -O2 -pthread -D_GNU_SOURCE -I"$WORK_DIR/old/include" \
    "$WORK_DIR/old"/src/config.c "$WORK_DIR/old"/src/cpu_stats.c "$WORK_DIR/old"/src/task.c \
    "$WORK_DIR/old"/src/task_queue.c "$WORK_DIR/old"/src/load_balancer.c "$WORK_DIR/old"/src/logger.c \
    "$REPO_ROOT/benchmarks/legacy-vs-pool/old_bench.c" \
    -lm -lrt -o "$WORK_DIR/old_bench"

echo "== Building the current tree (worker pool) and new_bench ==" >&2
cmake -S "$REPO_ROOT" -B "$WORK_DIR/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$WORK_DIR/build" --target cpu_balancer_core >/dev/null
gcc -std=c11 -Wall -Wextra -O2 -pthread -D_GNU_SOURCE -I"$REPO_ROOT/include" \
    "$REPO_ROOT/benchmarks/legacy-vs-pool/new_bench.c" \
    "$WORK_DIR/build/libcpu_balancer_core.a" \
    -lm -lrt -o "$WORK_DIR/new_bench"

echo "== Raw per-task dispatch overhead (pthread create+join vs. queue push+pop) ==" >&2
gcc -O2 -pthread -o "$WORK_DIR/thread_overhead" "$REPO_ROOT/benchmarks/legacy-vs-pool/thread_overhead.c"
gcc -std=c11 -O2 -pthread -D_GNU_SOURCE -I"$REPO_ROOT/include" \
    "$REPO_ROOT/benchmarks/legacy-vs-pool/queue_overhead.c" \
    "$WORK_DIR/build/libcpu_balancer_core.a" -lm -lrt -o "$WORK_DIR/queue_overhead"
"$WORK_DIR/thread_overhead"
"$WORK_DIR/queue_overhead"

echo >&2
printf "%-8s %-6s %-6s %-10s %-10s %-14s %s\n" \
    design cores tasks submitted completed completion_s throughput
for n in "${TASK_COUNTS[@]}"; do
    cd "$WORK_DIR"
    rm -f old_bench.log
    line=$("$WORK_DIR/old_bench" "$NUM_CORES" "$n" "$MIN_MS" "$MAX_MS" "$SEED")
    printf "old      %s\n" "$(echo "$line" | tr ',' ' ')"

    rm -f new_bench.log
    line=$("$WORK_DIR/new_bench" "$NUM_CORES" "$n" "$MIN_MS" "$MAX_MS" "$SEED")
    printf "new      %s\n" "$(echo "$line" | tr ',' ' ')"
done
