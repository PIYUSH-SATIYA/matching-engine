
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
# GoogleTest — locate the system-installed shared libraries.
# Inside the project Docker image:  apt-get install -y libgtest-dev
# On the host (Arch-based):         pacman -S gtest  (provides .so libs)
# ---------------------------------------------------------------------------
option(BUILD_SLAB_ALLOCATOR_TESTS "Build SlabAllocator GoogleTest tests when available" ON)
if(BUILD_SLAB_ALLOCATOR_TESTS)
    find_library(GTEST_LIB gtest)
    find_library(GTEST_MAIN_LIB gtest_main)
endif()

# ---------------------------------------------------------------------------
# Test target — Slab Allocator unit tests (Iteration 4, Member C)
# Build:  cmake .. && make test_slab_allocator
# Run:    ./test_slab_allocator  (or ctest)
# ---------------------------------------------------------------------------
if(BUILD_SLAB_ALLOCATOR_TESTS AND GTEST_LIB AND GTEST_MAIN_LIB)
    add_executable(test_slab_allocator
        tests/test_slab_allocator.cpp
    )
    target_include_directories(test_slab_allocator PRIVATE include)
    target_link_libraries(test_slab_allocator
        PRIVATE ${GTEST_LIB} ${GTEST_MAIN_LIB} pthread
    )

    enable_testing()
    add_test(NAME SlabAllocatorTests COMMAND test_slab_allocator)
elseif(BUILD_SLAB_ALLOCATOR_TESTS)
    message(STATUS "GoogleTest libraries not found; skipping SlabAllocator tests")
endif()

# Ring buffer tests
enable_testing()

add_executable(ring_buffer_test
    tests/test_ring_buffer.cpp
    src/ring_buffer.cpp
)

target_include_directories(ring_buffer_test PRIVATE include)
target_link_libraries(ring_buffer_test PRIVATE Threads::Threads)

add_test(NAME ring_buffer_test COMMAND ring_buffer_test)
