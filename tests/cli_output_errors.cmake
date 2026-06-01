if(NOT DEFINED WC_EXE)
  message(FATAL_ERROR "WC_EXE is required")
endif()
if(NOT DEFINED WC_TMP_DIR)
  message(FATAL_ERROR "WC_TMP_DIR is required")
endif()

if(NOT EXISTS "/dev/full")
  message(STATUS "Skipping /dev/full output-error checks")
  return()
endif()

file(MAKE_DIRECTORY "${WC_TMP_DIR}")
set(input "${WC_TMP_DIR}/input.txt")
file(WRITE "${input}" "alpha beta alpha\n")

function(expect_output_failure name)
  execute_process(${ARGN} RESULT_VARIABLE rc)
  if(rc EQUAL 0)
    message(FATAL_ERROR "${name}: expected non-zero exit on output failure")
  endif()
endfunction()

expect_output_failure(table_stdout
  COMMAND "${WC_EXE}" "${input}"
  OUTPUT_FILE "/dev/full"
  ERROR_QUIET)

expect_output_failure(json_stdout
  COMMAND "${WC_EXE}" "--format" "json" "${input}"
  OUTPUT_FILE "/dev/full"
  ERROR_QUIET)

expect_output_failure(tsv_summary_stderr
  COMMAND "${WC_EXE}" "--format" "tsv" "${input}"
  OUTPUT_QUIET
  ERROR_FILE "/dev/full")

expect_output_failure(help_stdout
  COMMAND "${WC_EXE}" "--help"
  OUTPUT_FILE "/dev/full"
  ERROR_QUIET)

expect_output_failure(version_stdout
  COMMAND "${WC_EXE}" "--version"
  OUTPUT_FILE "/dev/full"
  ERROR_QUIET)
