cmake_minimum_required(VERSION 3.16)
project(tinykv VERSION 1.0.0 LANGUAGES CXX)

find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(tinykv_core
    src/database.cpp
    src/protocol.cpp
    src/server.cpp
    src/wal.cpp
)
target_include_directories(tinykv_core PUBLIC include PRIVATE src)
target_link_libraries(tinykv_core PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(tinykv_core PUBLIC ws2_32)
endif()

if(MSVC)
    target_compile_options(tinykv_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(tinykv_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_executable(tinykv-server src/main.cpp)
target_link_libraries(tinykv-server PRIVATE tinykv_core)

add_executable(tinykv-cli src/cli.cpp)
target_link_libraries(tinykv-cli PRIVATE tinykv_core)

add_executable(tinykv-bench benchmarks/benchmark.cpp)
target_link_libraries(tinykv-bench PRIVATE tinykv_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(tinykv-tests tests/test_main.cpp)
    target_link_libraries(tinykv-tests PRIVATE tinykv_core)
    add_test(NAME tinykv-tests COMMAND tinykv-tests)
endif()
