cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED WC_C_COMPILER)
  message(FATAL_ERROR "WC_C_COMPILER is required")
endif()
if(NOT DEFINED WC_C_COMPILER_ID)
  message(FATAL_ERROR "WC_C_COMPILER_ID is required")
endif()
if(NOT DEFINED WC_SOURCE_DIR)
  message(FATAL_ERROR "WC_SOURCE_DIR is required")
endif()
if(NOT DEFINED WC_TMP_DIR)
  message(FATAL_ERROR "WC_TMP_DIR is required")
endif()

file(MAKE_DIRECTORY "${WC_TMP_DIR}")

set(wc_common_defs
    WC_HASH_STRONG=0
    WC_USE_LIBC_STRING=1
    WC_ENABLE_VALIDATE=0)

function(wc_expect_config_failure name)
  set(defs ${ARGN})

  if(WC_C_COMPILER_ID STREQUAL "MSVC")
    set(obj "${WC_TMP_DIR}/${name}.obj")
    set(cmd
        "${WC_C_COMPILER}"
        /nologo
        /TC
        /std:c11
        "/I${WC_SOURCE_DIR}"
        /c
        "${WC_SOURCE_DIR}/wordcount.c"
        "/Fo${obj}")
    foreach(def IN LISTS wc_common_defs defs)
      list(APPEND cmd "/D${def}")
    endforeach()
  else()
    set(obj "${WC_TMP_DIR}/${name}.o")
    set(cmd
        "${WC_C_COMPILER}"
        -std=c99
        -I
        "${WC_SOURCE_DIR}"
        -c
        "${WC_SOURCE_DIR}/wordcount.c"
        -o
        "${obj}")
    foreach(def IN LISTS wc_common_defs defs)
      list(APPEND cmd "-D${def}")
    endforeach()
  endif()

  execute_process(
    COMMAND ${cmd}
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)

  if(rc EQUAL 0)
    message(FATAL_ERROR "Invalid config '${name}' compiled successfully")
  endif()
endfunction()

wc_expect_config_failure(negative_max_word WC_MAX_WORD=-1 WC_STACK_BUFFER=0)
wc_expect_config_failure(negative_min_init_cap WC_MIN_INIT_CAP=-1)
wc_expect_config_failure(negative_min_block_sz WC_MIN_BLOCK_SZ=-1)
wc_expect_config_failure(negative_default_init_cap WC_DEFAULT_INIT_CAP=-1)
wc_expect_config_failure(negative_default_block_sz WC_DEFAULT_BLOCK_SZ=-1)

if(WC_C_COMPILER_ID MATCHES "Clang|GNU")
  wc_expect_config_failure(overaligned_u32
    "WC_U32_T=unsigned int __attribute__((aligned(32)))")
endif()
