foreach(var WC_EXE WC_TMP_DIR)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "${var} is required")
  endif()
endforeach()

file(REMOVE_RECURSE "${WC_TMP_DIR}")
file(MAKE_DIRECTORY "${WC_TMP_DIR}")

set(input "${WC_TMP_DIR}/input.txt")
file(WRITE "${input}" "Beta alpha beta\n")

function(expect_case name expected_rc expected_stdout expected_stderr)
  execute_process(${ARGN}
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
  if(NOT rc EQUAL expected_rc)
    message(FATAL_ERROR "${name}: expected rc ${expected_rc}, got ${rc}\nstdout=${out}\nstderr=${err}")
  endif()
  if(NOT out STREQUAL expected_stdout)
    message(FATAL_ERROR "${name}: stdout mismatch\nexpected=${expected_stdout}\nactual=${out}")
  endif()
  if(NOT err STREQUAL expected_stderr)
    message(FATAL_ERROR "${name}: stderr mismatch\nexpected=${expected_stderr}\nactual=${err}")
  endif()
endfunction()

expect_case(tsv_top1 0
  "rank\tcount\tword\n1\t2\tbeta\n"
  "Total: 3  Unique: 2  Filtered: 2  Shown: 1  Bytes: 16\n"
  COMMAND "${WC_EXE}" "--format" "tsv" "-n" "1" "${input}")

expect_case(json_filtered 0
  "{\"words\":[{\"rank\":1,\"count\":1,\"word\":\"alpha\"}],\"summary\":{\"total\":3,\"unique\":2,\"filtered\":1,\"displayed\":1,\"bytes\":16}}\n"
  ""
  COMMAND "${WC_EXE}" "--format" "json" "--min-len" "5" "${input}")

expect_case(quiet_summary 0
  "Total: 3  Unique: 2  Filtered: 1  Shown: 0  Bytes: 16\n"
  ""
  COMMAND "${WC_EXE}" "--quiet" "--min-len" "5" "${input}")

expect_case(invalid_env_limit 1
  ""
  "wc: invalid WC_MAX_BYTES value (must be non-negative integer)\n"
  COMMAND "${CMAKE_COMMAND}" -E env "WC_MAX_BYTES=-1" "${WC_EXE}" "${input}")

set(dash_input "${WC_TMP_DIR}/-")
file(WRITE "${dash_input}" "dash dash\n")
expect_case(literal_dash_file 0
  "{\"words\":[{\"rank\":1,\"count\":2,\"word\":\"dash\"}],\"summary\":{\"total\":2,\"unique\":1,\"filtered\":1,\"displayed\":1,\"bytes\":10}}\n"
  ""
  COMMAND "${WC_EXE}" "--format" "json" "-"
  WORKING_DIRECTORY "${WC_TMP_DIR}")

set(missing "${WC_TMP_DIR}/missing.txt")
execute_process(
  COMMAND "${WC_EXE}" "${input}" "${missing}"
  RESULT_VARIABLE missing_rc
  OUTPUT_VARIABLE missing_out
  ERROR_VARIABLE missing_err)
if(NOT missing_rc EQUAL 1)
  message(FATAL_ERROR "partial_input_failure_suppresses_output: expected rc 1, got ${missing_rc}")
endif()
if(NOT missing_out STREQUAL "")
  message(FATAL_ERROR "partial_input_failure_suppresses_output: expected empty stdout, got ${missing_out}")
endif()
if(NOT missing_err MATCHES "^wc: \"${missing}\": .+\n$")
  message(FATAL_ERROR "partial_input_failure_suppresses_output: unexpected stderr: ${missing_err}")
endif()

set(control_path "${WC_TMP_DIR}/missing\nname.txt")
execute_process(
  COMMAND "${WC_EXE}" "${control_path}"
  RESULT_VARIABLE control_rc
  OUTPUT_VARIABLE control_out
  ERROR_VARIABLE control_err)
if(NOT control_rc EQUAL 1)
  message(FATAL_ERROR "quoted_control_path: expected rc 1, got ${control_rc}")
endif()
if(NOT control_out STREQUAL "")
  message(FATAL_ERROR "quoted_control_path: expected empty stdout, got ${control_out}")
endif()
if(NOT control_err MATCHES "\\\\x0a")
  message(FATAL_ERROR "quoted_control_path: stderr did not escape newline: ${control_err}")
endif()
