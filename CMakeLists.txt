cmake_minimum_required(VERSION 3.20)

project(
    FlashKV
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Terminal-based key-value store
add_executable(
    flashkv
    src/main.cpp
    src/database.cpp
)

target_include_directories(
    flashkv PRIVATE include
)

target_compile_options(
    flashkv PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# Redis-compatible TCP server
add_executable(
    flashkv_server
    src/tcp_server.cpp
    src/database.cpp
    src/resp.cpp
    src/aof.cpp
)

target_include_directories(
    flashkv_server PRIVATE include
)

target_compile_options(
    flashkv_server PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)