cmake_minimum_required(VERSION 3.14)
project(advanced_cpu_balancer C)

# Set C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Build type configuration
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
add_compile_options(-Wall -Wextra -Wpedantic -O2 -pthread)

# Define _GNU_SOURCE to enable GNU-specific extensions like CPU_SET, CPU_ZERO, etc.
add_compile_definitions(_GNU_SOURCE)

# Check for required libraries
find_package(Threads REQUIRED)

# Everything except an entry point: shared by both executables below.
set(CORE_SOURCES
    src/config.c
    src/cpu_stats.c
    src/task.c
    src/task_queue.c
    src/load_balancer.c
    src/logger.c
)

set(HEADERS
    include/config.h
    include/cpu_stats.h
    include/task.h
    include/task_queue.h
    include/load_balancer.h
    include/logger.h
)

add_library(cpu_balancer_core STATIC ${CORE_SOURCES})

target_include_directories(cpu_balancer_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(cpu_balancer_core
    PUBLIC
        Threads::Threads
        m
        rt
)

# Main executable
add_executable(cpu_balancer src/main.c)
target_link_libraries(cpu_balancer PRIVATE cpu_balancer_core)

# Benchmark: compares the three scheduling policies against each other.
add_executable(cpu_balancer_bench src/benchmark.c)
target_link_libraries(cpu_balancer_bench PRIVATE cpu_balancer_core)

# Tests: one small, focused binary per concern rather than one monolithic
# runner, so `ctest --output-on-failure` points straight at which concern
# broke instead of just "the test suite failed".
enable_testing()

set(TEST_SOURCES
    tests/test_task_queue_priority.c
    tests/test_task_queue_capacity.c
    tests/test_core_queue.c
    tests/test_select_cpu.c
    tests/test_cpu_stats.c
    tests/test_task_ownership.c
    tests/test_shutdown.c
    tests/test_concurrent_producers.c
    tests/test_drain_lock_hygiene.c
)

foreach(test_src ${TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE cpu_balancer_core)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Installation rules
install(TARGETS cpu_balancer cpu_balancer_bench
    RUNTIME DESTINATION bin
)

install(FILES
    config/cpu_balancer.conf
    DESTINATION etc/cpu_balancer
)
