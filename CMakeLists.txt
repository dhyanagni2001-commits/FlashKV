cmake_minimum_required(VERSION 3.20)

project(
    FlashKV
    VERSION 0.2.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(FLASHKV_WARNINGS -Wall -Wextra -Wpedantic)

# Storage engine, protocol, commands and persistence
add_library(
    flashkv_core STATIC
    src/database.cpp
    src/resp.cpp
    src/commands.cpp
    src/aof.cpp
    src/reply_format.cpp
)

target_include_directories(flashkv_core PUBLIC include)
target_compile_options(flashkv_core PRIVATE ${FLASHKV_WARNINGS})

# Terminal-based key-value shell
add_executable(flashkv src/main.cpp)
target_link_libraries(flashkv PRIVATE flashkv_core)
target_compile_options(flashkv PRIVATE ${FLASHKV_WARNINGS})

# Redis-compatible TCP server
add_executable(flashkv_server src/tcp_server.cpp)
target_link_libraries(flashkv_server PRIVATE flashkv_core)
target_compile_options(flashkv_server PRIVATE ${FLASHKV_WARNINGS})

# Network load generator
find_package(Threads REQUIRED)

add_executable(flashkv_benchmark benchmarks/benchmark.cpp)
target_link_libraries(flashkv_benchmark PRIVATE flashkv_core Threads::Threads)
target_compile_options(flashkv_benchmark PRIVATE ${FLASHKV_WARNINGS})

# Tests
enable_testing()

add_executable(flashkv_unit_tests tests/unit_tests.cpp)
target_link_libraries(flashkv_unit_tests PRIVATE flashkv_core)
target_compile_options(flashkv_unit_tests PRIVATE ${FLASHKV_WARNINGS})

add_test(NAME unit_tests COMMAND flashkv_unit_tests)

find_package(Python3 COMPONENTS Interpreter)

if(Python3_FOUND)
    add_test(
        NAME integration_tests
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/integration_test.py
                --server $<TARGET_FILE:flashkv_server>
    )
endif()
