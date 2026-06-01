foreach(var WC_BUILD_DIR WC_INSTALL_PREFIX WC_PKGCONFIG_DIR WC_PKG_CONFIG
            WC_C_COMPILER)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "${var} is required")
  endif()
endforeach()

file(REMOVE_RECURSE "${WC_INSTALL_PREFIX}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${WC_BUILD_DIR}"
          --prefix "${WC_INSTALL_PREFIX}"
  RESULT_VARIABLE install_rc
  OUTPUT_VARIABLE install_out
  ERROR_VARIABLE install_err)
if(NOT install_rc EQUAL 0)
  message(FATAL_ERROR
          "install failed (${install_rc})\n${install_out}\n${install_err}")
endif()

set(ENV{PKG_CONFIG_PATH} "${WC_INSTALL_PREFIX}/${WC_PKGCONFIG_DIR}")

execute_process(
  COMMAND "${WC_PKG_CONFIG}" --cflags wordcount
  RESULT_VARIABLE cflags_rc
  OUTPUT_VARIABLE cflags
  ERROR_VARIABLE cflags_err)
if(NOT cflags_rc EQUAL 0)
  message(FATAL_ERROR "pkg-config --cflags failed\n${cflags_err}")
endif()

execute_process(
  COMMAND "${WC_PKG_CONFIG}" --libs wordcount
  RESULT_VARIABLE libs_rc
  OUTPUT_VARIABLE libs
  ERROR_VARIABLE libs_err)
if(NOT libs_rc EQUAL 0)
  message(FATAL_ERROR "pkg-config --libs failed\n${libs_err}")
endif()

string(STRIP "${cflags}" cflags)
string(STRIP "${libs}" libs)
separate_arguments(cflag_args UNIX_COMMAND "${cflags}")
separate_arguments(lib_args UNIX_COMMAND "${libs}")

file(WRITE "${WC_INSTALL_PREFIX}/consumer.c"
     "#include <wordcount.h>\n"
     "int main(void) { return wc_version()[0] ? 0 : 1; }\n")

execute_process(
  COMMAND "${WC_C_COMPILER}" -std=c99 ${cflag_args}
          "${WC_INSTALL_PREFIX}/consumer.c" ${lib_args}
          -o "${WC_INSTALL_PREFIX}/consumer"
  RESULT_VARIABLE compile_rc
  OUTPUT_VARIABLE compile_out
  ERROR_VARIABLE compile_err)
if(NOT compile_rc EQUAL 0)
  message(FATAL_ERROR
          "consumer compile failed\ncflags=${cflags}\nlibs=${libs}\n"
          "${compile_out}\n${compile_err}")
endif()

execute_process(
  COMMAND "${WC_INSTALL_PREFIX}/consumer"
  RESULT_VARIABLE run_rc)
if(NOT run_rc EQUAL 0)
  message(FATAL_ERROR "consumer exited ${run_rc}")
endif()
