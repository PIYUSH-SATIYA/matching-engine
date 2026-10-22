
cmake_minimum_required(VERSION 3.16)
project(matching_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_SANITIZERS "Build with ASan/UBSan" OFF)
option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)

if(ENABLE_SANITIZERS AND ENABLE_TSAN)
    message(FATAL_ERROR
        "ASan/UBSan and ThreadSanitizer must be built separately.")
endif()

if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -g -O0)
    add_link_options(-fsanitize=address,undefined)
endif()

if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -g -O1)
    add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)

# Main application
add_executable(matching_engine
    src/main.cpp
    src/order_book.cpp
    src/ring_buffer.cpp
)

target_include_directories(matching_engine PRIVATE include)
target_link_libraries(matching_engine PRIVATE Threads::Threads)

# Ring buffer tests
enable_testing()

add_executable(ring_buffer_test
    tests/test_ring_buffer.cpp
    src/ring_buffer.cpp
)

target_include_directories(ring_buffer_test PRIVATE include)
target_link_libraries(ring_buffer_test PRIVATE Threads::Threads)

add_test(NAME ring_buffer_test COMMAND ring_buffer_test)
