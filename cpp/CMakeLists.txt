cmake_minimum_required(VERSION 3.16)
project(HFT_System LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler-specific optimizations for HFT
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # High-performance optimizations
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native -mtune=native")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -ffast-math -funroll-loops")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -flto")  # Link-time optimization
    
    # Additional flags for low-latency optimization
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions")  # Disable exceptions for performance
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")       # Disable RTTI for performance
    
    # Debug flags
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address")  # Address sanitizer
endif()

# Set default build type to Release for performance
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Include directories
include_directories(include)

# Source files
set(SOURCES
    src/memory_pool.cpp
    src/latency_tracker.cpp
    src/order_manager.cpp
    src/orderbook_engine.cpp
    src/signal_engine.cpp
    src/market_data_feed.cpp
)

# Create static library for the HFT core
add_library(hft_core STATIC ${SOURCES})

# Set target properties
set_target_properties(hft_core PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# Thread support for concurrent operations
find_package(Threads REQUIRED)
target_link_libraries(hft_core PRIVATE Threads::Threads)

# =============================================================================
# TESTING SETUP
# =============================================================================

# Find Google Test
find_package(GTest QUIET)
if(GTest_FOUND OR GTEST_FOUND)
    message(STATUS "Google Test found - building unit tests")
    enable_testing()
    
    # Individual component tests (with exceptions enabled for Google Test)
    add_executable(test_memory tests/test_memory.cpp)
    target_link_libraries(test_memory PRIVATE hft_core GTest::gtest GTest::gtest_main Threads::Threads)
    target_compile_options(test_memory PRIVATE -fexceptions -frtti)  # Enable exceptions for tests
    add_test(NAME MemoryTests COMMAND test_memory)
    
    add_executable(test_latency tests/test_latency.cpp)
    target_link_libraries(test_latency PRIVATE hft_core GTest::gtest GTest::gtest_main Threads::Threads)
    target_compile_options(test_latency PRIVATE -fexceptions -frtti)  # Enable exceptions for tests
    add_test(NAME LatencyTests COMMAND test_latency)
    
    add_executable(test_order_manager tests/test_order_manager.cpp)
    target_link_libraries(test_order_manager PRIVATE hft_core GTest::gtest GTest::gtest_main Threads::Threads)
    target_compile_options(test_order_manager PRIVATE -fexceptions -frtti)  # Enable exceptions for tests
    add_test(NAME OrderManagerTests COMMAND test_order_manager)
    
    # Performance benchmarks
    add_executable(performance_benchmark tests/performance_benchmark.cpp)
    target_link_libraries(performance_benchmark PRIVATE hft_core GTest::gtest GTest::gtest_main Threads::Threads)
    target_compile_options(performance_benchmark PRIVATE -fexceptions -frtti)  # Enable exceptions for tests
    
    # Create a test target for running all tests
    add_custom_target(run_tests
        COMMAND ${CMAKE_CTEST_COMMAND} --verbose
        DEPENDS test_memory test_latency test_order_manager
        COMMENT "Running all unit tests"
    )
    
else()
    message(STATUS "Google Test not found - skipping unit tests")
    message(STATUS "Install with: sudo apt-get install libgtest-dev")
endif()

# Optional: Python bindings (if pybind11 is available)
find_package(pybind11 QUIET)
if(pybind11_FOUND)
    message(STATUS "pybind11 found - building Python bindings")
    pybind11_add_module(hft_bindings ../bindings/python_bindings.cpp)
    target_link_libraries(hft_bindings PRIVATE hft_core)
    
    # Compiler-specific flags for bindings
    target_compile_definitions(hft_bindings PRIVATE VERSION_INFO=${EXAMPLE_VERSION_INFO})
else()
    message(STATUS "pybind11 not found - skipping Python bindings")
endif()

# Note: Individual test executables are defined in the testing section above

# Install targets
install(TARGETS hft_core
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.hpp"
)

# CPack configuration for distribution
set(CPACK_PACKAGE_NAME "HFT_System")
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_PACKAGE_DESCRIPTION "High-Frequency Trading System")
set(CPACK_PACKAGE_CONTACT "developer@hft-system.com")

include(CPack)

# Custom targets for development
add_custom_target(format
    COMMAND find ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include 
            -name "*.cpp" -o -name "*.hpp" | xargs clang-format -i
    COMMENT "Formatting source code"
)

add_custom_target(lint
    COMMAND find ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include
            -name "*.cpp" -o -name "*.hpp" | xargs clang-tidy
    COMMENT "Running static analysis"
)

# Performance profiling support
option(ENABLE_PROFILING "Enable profiling support" OFF)
if(ENABLE_PROFILING)
    target_compile_definitions(hft_core PRIVATE ENABLE_PROFILING=1)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(hft_core PRIVATE -pg)
        target_link_options(hft_core PRIVATE -pg)
    endif()
endif()

# Memory debugging support
option(ENABLE_MEMORY_DEBUG "Enable memory debugging" OFF)
if(ENABLE_MEMORY_DEBUG)
    target_compile_definitions(hft_core PRIVATE ENABLE_MEMORY_DEBUG=1)
endif()

# Print configuration summary
message(STATUS "")
message(STATUS "HFT System Configuration Summary:")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Install Prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Profiling: ${ENABLE_PROFILING}")
message(STATUS "  Memory Debug: ${ENABLE_MEMORY_DEBUG}")
if(pybind11_FOUND)
    message(STATUS "  Python Bindings: Enabled")
else()
    message(STATUS "  Python Bindings: Disabled (pybind11 not found)")
endif()
message(STATUS "")
