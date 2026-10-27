cmake_minimum_required(VERSION 3.16)
project(matching_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Sanitizer build options
# ASan/UBSan and TSan must be enabled in separate builds.
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

find_package(Threads REQUIRED)

# Main application
add_executable(matching_engine
    src/main.cpp
    src/order_book.cpp
    src/ring_buffer.cpp
    src/slab_allocator.cpp
)
target_include_directories(matching_engine PRIVATE include)
target_link_libraries(matching_engine PRIVATE Threads::Threads)

# Tests
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

# Benchmark target — SlabAllocator vs new/delete latency comparison.
# Keep the target name available for build scripts even before the benchmark
# source lands. A real benchmark executable is created whenever its source is
# present.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bench/bench_main.cpp")
    add_executable(bench_slab_allocator bench/bench_main.cpp)
    target_include_directories(bench_slab_allocator PRIVATE include)
    target_link_libraries(bench_slab_allocator PRIVATE Threads::Threads)
    target_compile_options(bench_slab_allocator PRIVATE -O2 -DNDEBUG)
else()
    message(STATUS "bench/bench_main.cpp not found; benchmark executable is unavailable")
    add_custom_target(bench_slab_allocator
        COMMAND ${CMAKE_COMMAND} -E echo
            "bench_slab_allocator unavailable: add bench/bench_main.cpp"
    )
endif()
