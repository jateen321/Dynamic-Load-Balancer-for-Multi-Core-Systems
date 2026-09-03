.PHONY: all build clean rebuild run bench tsan asan install

BUILD_DIR ?= build
CORES     ?= 4
TASKS     ?= 20

BENCH_CORES ?= 4
BENCH_TASKS ?= 60

all: build

build:
	cmake -S . -B $(BUILD_DIR)
	cmake --build $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) build-tsan build-asan

rebuild: clean build

# Smoke run: $(CORES) cores, $(TASKS) tasks
run: build
	./$(BUILD_DIR)/cpu_balancer $(CORES) $(TASKS)

# Compare scheduling policies: $(BENCH_CORES) cores, $(BENCH_TASKS) tasks/policy
bench: build
	./$(BUILD_DIR)/cpu_balancer_bench $(BENCH_CORES) $(BENCH_TASKS)

# Data-race detector build
tsan:
	cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
	      -DCMAKE_C_FLAGS="-fsanitize=thread -g -O1"
	cmake --build build-tsan

# Memory-error detector build
asan:
	cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
	      -DCMAKE_C_FLAGS="-fsanitize=address,undefined -g -O1"
	cmake --build build-asan

install: build
	cmake --install $(BUILD_DIR)
