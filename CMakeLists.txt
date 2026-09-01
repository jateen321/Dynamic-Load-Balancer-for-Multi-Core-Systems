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

# Define source and header files
set(SOURCES
    src/main.c
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

# Add executable
add_executable(cpu_balancer ${SOURCES})

# Include directories
target_include_directories(cpu_balancer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Link libraries
target_link_libraries(cpu_balancer
    PRIVATE
        Threads::Threads
        m
        rt
)

# Installation rules
install(TARGETS cpu_balancer
    RUNTIME DESTINATION bin
)

install(FILES
    config/cpu_balancer.conf
    DESTINATION etc/cpu_balancer
)