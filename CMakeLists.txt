
cmake_minimum_required(VERSION 3.16)
project(matching_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Sanitizer build option
# Usage:  cmake -DENABLE_SANITIZERS=ON ..
# Note:   ASan and TSan cannot coexist — build twice if you need both.
# ---------------------------------------------------------------------------
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

# Benchmarks are meaningless in an unoptimized build: default to Release unless
# a sanitizer build was requested explicitly.
if(NOT CMAKE_BUILD_TYPE AND NOT ENABLE_SANITIZERS AND NOT ENABLE_TSAN)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# Main application
add_executable(matching_engine
    src/main.cpp
    src/order_book.cpp
    src/ring_buffer.cpp
)
target_include_directories(matching_engine PRIVATE include)
target_link_libraries(matching_engine PRIVATE pthread Threads::Threads)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

add_executable(slab_allocator_test
    tests/test_slab_allocator.cpp
)

target_include_directories(slab_allocator_test PRIVATE include)

add_test(NAME slab_allocator_test COMMAND slab_allocator_test)

add_executable(ring_buffer_test
    tests/test_ring_buffer.cpp
    src/ring_buffer.cpp
)

target_include_directories(ring_buffer_test PRIVATE include)
target_link_libraries(ring_buffer_test PRIVATE Threads::Threads)

add_test(NAME ring_buffer_test COMMAND ring_buffer_test)

# --- order book (V1) -------------------------------------------------------
add_executable(order_book_test
    tests/test_order_book.cpp
    src/order_book.cpp
)
target_include_directories(order_book_test PRIVATE include)
add_test(NAME order_book_test COMMAND order_book_test)

# --- order book V3 (header-only) + differential test vs V1 -----------------
add_executable(order_book_fast_test
    tests/test_order_book_fast.cpp
    src/order_book.cpp
)
target_include_directories(order_book_fast_test PRIVATE include)
add_test(NAME order_book_fast_test COMMAND order_book_fast_test)

# --- matching engine (header-only), tested on V1 and V3 --------------------
add_executable(matching_engine_test
    tests/test_matching_engine.cpp
    src/order_book.cpp
)
target_include_directories(matching_engine_test PRIVATE include)
add_test(NAME matching_engine_test COMMAND matching_engine_test)

# --- SPSC lock-free ring buffer (header-only) ------------------------------
add_executable(spsc_ring_buffer_test tests/test_spsc_ring_buffer.cpp)
target_include_directories(spsc_ring_buffer_test PRIVATE include)
target_link_libraries(spsc_ring_buffer_test PRIVATE Threads::Threads)
add_test(NAME spsc_ring_buffer_test COMMAND spsc_ring_buffer_test)

# ---------------------------------------------------------------------------
# Benchmarks (build with the default Release type; do NOT run under sanitizers)
#   ./bench_order_book 2000000
#   ./bench_pipeline 2000000 500000 2 3     # orders rate cpu_producer cpu_engine
# ---------------------------------------------------------------------------
add_executable(bench_order_book
    bench/bench_order_book.cpp
    src/order_book.cpp
)
target_include_directories(bench_order_book PRIVATE include)

add_executable(bench_pipeline
    bench/bench_pipeline.cpp
    src/order_book.cpp
    src/ring_buffer.cpp
)
target_include_directories(bench_pipeline PRIVATE include)
target_link_libraries(bench_pipeline PRIVATE Threads::Threads)
