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

set(cmake_consumer "${WC_INSTALL_PREFIX}/cmake-consumer")
get_filename_component(WC_LIBDIR "${WC_PKGCONFIG_DIR}" DIRECTORY)
set(wordcount_config_dir "${WC_INSTALL_PREFIX}/${WC_LIBDIR}/cmake/wordcount")
if(NOT EXISTS "${wordcount_config_dir}/wordcountConfig.cmake")
  message(FATAL_ERROR "Installed CMake config is missing: ${wordcount_config_dir}/wordcountConfig.cmake")
endif()
file(MAKE_DIRECTORY "${cmake_consumer}")
file(WRITE "${cmake_consumer}/CMakeLists.txt"
     "cmake_minimum_required(VERSION 3.20)\n"
     "project(wordcount_cmake_consumer LANGUAGES C)\n"
     "find_package(wordcount CONFIG REQUIRED)\n"
     "add_executable(consumer_static main.c)\n"
     "target_link_libraries(consumer_static PRIVATE wordcount::wordcount)\n"
     "if(TARGET wordcount::wordcount_shared)\n"
     "  add_executable(consumer_shared main.c)\n"
     "  target_link_libraries(consumer_shared PRIVATE wordcount::wordcount_shared)\n"
     "  set_target_properties(consumer_shared PROPERTIES BUILD_RPATH \"$<TARGET_FILE_DIR:wordcount::wordcount_shared>\")\n"
     "endif()\n")
file(WRITE "${cmake_consumer}/main.c"
     "#include <wordcount.h>\n"
     "int main(void) { return wc_version()[0] ? 0 : 1; }\n")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${cmake_consumer}"
          -B "${cmake_consumer}/build" -G Ninja
          "-Dwordcount_DIR:PATH=${wordcount_config_dir}"
  RESULT_VARIABLE cmake_config_rc
  OUTPUT_VARIABLE cmake_config_out
  ERROR_VARIABLE cmake_config_err)
if(NOT cmake_config_rc EQUAL 0)
  message(FATAL_ERROR
          "CMake consumer configure failed\nwordcount_DIR=${wordcount_config_dir}\n"
          "${cmake_config_out}\n${cmake_config_err}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${cmake_consumer}/build"
  RESULT_VARIABLE cmake_build_rc
  OUTPUT_VARIABLE cmake_build_out
  ERROR_VARIABLE cmake_build_err)
if(NOT cmake_build_rc EQUAL 0)
  message(FATAL_ERROR
          "CMake consumer build failed\n${cmake_build_out}\n${cmake_build_err}")
endif()

set(cmake_consumer_exes consumer_static)
if(EXISTS "${cmake_consumer}/build/consumer_shared")
  list(APPEND cmake_consumer_exes consumer_shared)
endif()

foreach(exe IN LISTS cmake_consumer_exes)
  execute_process(
    COMMAND "${cmake_consumer}/build/${exe}"
    RESULT_VARIABLE cmake_run_rc)
  if(NOT cmake_run_rc EQUAL 0)
    message(FATAL_ERROR "CMake consumer ${exe} exited ${cmake_run_rc}")
  endif()
endforeach()
