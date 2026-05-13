cmake_minimum_required(VERSION 3.12)
project(elh VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)

# Library
add_library(elh src/elh.c)
target_include_directories(elh PUBLIC include)
# Enable AVX2 if available, fall back to scalar
include(CheckCCompilerFlag)
check_c_compiler_flag(-mavx2 HAVE_AVX2)
if(HAVE_AVX2)
    target_compile_options(elh PRIVATE -O2 -Wall -Wextra -mavx2)
    target_compile_definitions(elh PUBLIC ELH_HAVE_AVX2)
    message(STATUS "ELH: AVX2 enabled")
else()
    target_compile_options(elh PRIVATE -O2 -Wall -Wextra)
    message(STATUS "ELH: AVX2 not available, using scalar path")
endif()

# Benchmark tool
add_executable(elh_bench bench/bench.c)
target_link_libraries(elh_bench elh)
target_compile_options(elh_bench PRIVATE -O2)

# Install
install(TARGETS elh ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
install(FILES include/elh.h DESTINATION include)

# Correctness tests
add_executable(elh_test tests/test_correctness.c)
target_link_libraries(elh_test elh)
enable_testing()
add_test(NAME correctness COMMAND elh_test)

# Streaming tests
add_executable(elh_stream_test tests/test_streaming.c)
target_link_libraries(elh_stream_test elh)
add_test(NAME streaming COMMAND elh_stream_test)
