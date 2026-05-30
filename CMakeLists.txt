cmake_minimum_required(VERSION 3.12)
project(elh VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)

# Library
set(ELH_SOURCES src/elh.c src/elh_frame.c)
add_library(elh ${ELH_SOURCES})
target_include_directories(elh PUBLIC include)
add_library(elh_shared SHARED ${ELH_SOURCES})
set_target_properties(elh_shared PROPERTIES OUTPUT_NAME elh)
target_include_directories(elh_shared PUBLIC include)

# Clean upstream LZ4 baseline, pinned in third_party/lz4.
set(LZ4_CLEAN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lz4")
add_library(lz4_clean STATIC "${LZ4_CLEAN_ROOT}/lib/lz4.c")
target_include_directories(lz4_clean PUBLIC "${LZ4_CLEAN_ROOT}/lib")
target_compile_options(lz4_clean PRIVATE -O2)
add_library(lz4_clean_shared SHARED "${LZ4_CLEAN_ROOT}/lib/lz4.c")
set_target_properties(lz4_clean_shared PROPERTIES OUTPUT_NAME lz4_clean)
target_include_directories(lz4_clean_shared PUBLIC "${LZ4_CLEAN_ROOT}/lib")
target_compile_options(lz4_clean_shared PRIVATE -O2)
# Enable AVX2 if available, fall back to scalar
include(CheckCCompilerFlag)
check_c_compiler_flag(-mavx2 HAVE_AVX2)
if(HAVE_AVX2)
    target_compile_options(elh PRIVATE -O2 -Wall -Wextra -mavx2)
    target_compile_options(elh_shared PRIVATE -O2 -Wall -Wextra -mavx2)
    target_compile_definitions(elh PUBLIC ELH_HAVE_AVX2)
    target_compile_definitions(elh_shared PUBLIC ELH_HAVE_AVX2)
    message(STATUS "ELH: AVX2 enabled")
else()
    target_compile_options(elh PRIVATE -O2 -Wall -Wextra)
    target_compile_options(elh_shared PRIVATE -O2 -Wall -Wextra)
    message(STATUS "ELH: AVX2 not available, using scalar path")
endif()

# Benchmark tool
add_executable(elh_bench bench/bench.c)
target_link_libraries(elh_bench elh)
target_compile_options(elh_bench PRIVATE -O2)

# Frame CLI
add_executable(elh_cli src/elh_cli.c)
target_link_libraries(elh_cli elh)
target_compile_options(elh_cli PRIVATE -O2)

# Examples
add_executable(elh_frame_example examples/frame_roundtrip.c)
target_link_libraries(elh_frame_example elh)
target_compile_options(elh_frame_example PRIVATE -O2)

# In-process ELH vs clean upstream LZ4 comparison benchmark.
add_executable(elh_compare bench/bench_compare.c)
target_link_libraries(elh_compare elh lz4_clean)
target_compile_options(elh_compare PRIVATE -O2)

# Install
install(TARGETS elh elh_shared lz4_clean lz4_clean_shared ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(FILES include/elh.h include/elh_frame.h DESTINATION include)

# Correctness tests
add_executable(elh_test tests/test_correctness.c)
target_link_libraries(elh_test elh)
enable_testing()
add_test(NAME correctness COMMAND elh_test)

# Streaming tests
add_executable(elh_stream_test tests/test_streaming.c)
target_link_libraries(elh_stream_test elh)
add_test(NAME streaming COMMAND elh_stream_test)

# Frame tests
add_executable(elh_frame_test tests/test_frame.c)
target_link_libraries(elh_frame_test elh)
add_test(NAME frame COMMAND elh_frame_test)

# CLI smoke test
add_test(NAME cli_roundtrip
         COMMAND ${CMAKE_COMMAND}
                 -DCLI=$<TARGET_FILE:elh_cli>
                 -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/cli_test
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.cmake)

add_test(NAME frame_example COMMAND elh_frame_example)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME python_bindings
             COMMAND ${CMAKE_COMMAND}
                     -E env
                     ELH_LIBRARY=$<TARGET_FILE:elh_shared>
                     PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/python
                     ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/python/test_elh.py)
    add_test(NAME python_lz4_baseline
             COMMAND ${CMAKE_COMMAND}
                     -E env
                     LZ4_LIBRARY=$<TARGET_FILE:lz4_clean_shared>
                     PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/python
                     ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/python/test_lz4_baseline.py)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import fastapi"
        RESULT_VARIABLE HAVE_FASTAPI
        OUTPUT_QUIET
        ERROR_QUIET)
    if(HAVE_FASTAPI EQUAL 0)
        add_test(NAME api_tests
                 COMMAND ${CMAKE_COMMAND}
                         -E env
                         ELH_LIBRARY=$<TARGET_FILE:elh_shared>
                         LZ4_LIBRARY=$<TARGET_FILE:lz4_clean_shared>
                         PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/python:${CMAKE_CURRENT_SOURCE_DIR}/api
                         ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/api/test_api.py)
    endif()
endif()
