# CMake generated Testfile for 
# Source directory: /mnt/c/Users/donmi/Music/comp-dts/elh
# Build directory: /mnt/c/Users/donmi/Music/comp-dts/elh/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(correctness "/mnt/c/Users/donmi/Music/comp-dts/elh/build/elh_test")
set_tests_properties(correctness PROPERTIES  _BACKTRACE_TRIPLES "/mnt/c/Users/donmi/Music/comp-dts/elh/CMakeLists.txt;60;add_test;/mnt/c/Users/donmi/Music/comp-dts/elh/CMakeLists.txt;0;")
add_test(streaming "/mnt/c/Users/donmi/Music/comp-dts/elh/build/elh_stream_test")
set_tests_properties(streaming PROPERTIES  _BACKTRACE_TRIPLES "/mnt/c/Users/donmi/Music/comp-dts/elh/CMakeLists.txt;65;add_test;/mnt/c/Users/donmi/Music/comp-dts/elh/CMakeLists.txt;0;")
add_test(frame "/mnt/c/Users/donmi/Music/comp-dts/elh/build/elh_frame_test")
set_tests_properties(frame PROPERTIES  _BACKTRACE_TRIPLES "/mnt/c/Users/donmi/Music/comp-dts/elh/CMakeLists.txt;70;add_test;/mnt/c/Users/donmi/Music/comp-dts/elh/CMakeLists.txt;0;")
add_test(cli_roundtrip "/usr/bin/cmake" "-DCLI=/mnt/c/Users/donmi/Music/comp-dts/elh/build/elh_cli" "-DWORKDIR=/mnt/c/Users/donmi/Music/comp-dts/elh/build/cli_test" "-P" "/mnt/c/Users/donmi/Music/comp-dts/elh/tests/test_cli.cmake")
set_tests_properties(cli_roundtrip PROPERTIES  _BACKTRACE_TRIPLES "/mnt/c/Users/donmi/Music/comp-dts/elh/CMakeLists.txt;73;add_test;/mnt/c/Users/donmi/Music/comp-dts/elh/CMakeLists.txt;0;")
add_test(frame_example "/mnt/c/Users/donmi/Music/comp-dts/elh/build/elh_frame_example")
set_tests_properties(frame_example PROPERTIES  _BACKTRACE_TRIPLES "/mnt/c/Users/donmi/Music/comp-dts/elh/CMakeLists.txt;79;add_test;/mnt/c/Users/donmi/Music/comp-dts/elh/CMakeLists.txt;0;")
add_test(python_bindings "/usr/bin/cmake" "-E" "env" "ELH_LIBRARY=/mnt/c/Users/donmi/Music/comp-dts/elh/build/libelh.so" "PYTHONPATH=/mnt/c/Users/donmi/Music/comp-dts/elh/python" "/usr/bin/python3" "/mnt/c/Users/donmi/Music/comp-dts/elh/python/test_elh.py")
set_tests_properties(python_bindings PROPERTIES  _BACKTRACE_TRIPLES "/mnt/c/Users/donmi/Music/comp-dts/elh/CMakeLists.txt;83;add_test;/mnt/c/Users/donmi/Music/comp-dts/elh/CMakeLists.txt;0;")
