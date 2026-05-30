file(MAKE_DIRECTORY "${WORKDIR}")

set(INPUT "${WORKDIR}/input.txt")
set(COMPRESSED "${WORKDIR}/input.elh")
set(OUTPUT "${WORKDIR}/output.txt")
set(COMPRESSED_SMALL "${WORKDIR}/input-small.elh")
set(OUTPUT_SMALL "${WORKDIR}/output-small.txt")

file(WRITE "${INPUT}" "")
foreach(i RANGE 1 2000)
  file(APPEND "${INPUT}" "ts=${i} service=api worker=3 level=info message=hello-elastic-hashing\n")
endforeach()

execute_process(
  COMMAND "${CLI}" -c --chunk 4096 -k 4 --overflow 1 "${INPUT}" "${COMPRESSED}"
  RESULT_VARIABLE rc
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "elh_cli compression failed")
endif()

execute_process(
  COMMAND "${CLI}" -d "${COMPRESSED}" "${OUTPUT}"
  RESULT_VARIABLE rc
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "elh_cli decompression failed")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files "${INPUT}" "${OUTPUT}"
  RESULT_VARIABLE rc
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "elh_cli roundtrip mismatch")
endif()

execute_process(
  COMMAND "${CLI}" -c --chunk 128 -k 2 "${INPUT}" "${COMPRESSED_SMALL}"
  RESULT_VARIABLE rc
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "elh_cli small-chunk compression failed")
endif()

execute_process(
  COMMAND "${CLI}" -d "${COMPRESSED_SMALL}" "${OUTPUT_SMALL}"
  RESULT_VARIABLE rc
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "elh_cli small-chunk decompression failed")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files "${INPUT}" "${OUTPUT_SMALL}"
  RESULT_VARIABLE rc
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "elh_cli small-chunk roundtrip mismatch")
endif()
