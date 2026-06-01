if(NOT DEFINED WC_C_COMPILER)
  message(FATAL_ERROR "WC_C_COMPILER is required")
endif()
if(NOT DEFINED WC_NM)
  message(FATAL_ERROR "WC_NM is required")
endif()
if(NOT DEFINED WC_SOURCE_DIR)
  message(FATAL_ERROR "WC_SOURCE_DIR is required")
endif()
if(NOT DEFINED WC_TMP_DIR)
  message(FATAL_ERROR "WC_TMP_DIR is required")
endif()

file(MAKE_DIRECTORY "${WC_TMP_DIR}")

set(use_limits_src "${WC_TMP_DIR}/use_limits.c")
file(WRITE "${use_limits_src}" [=[
#define WC_STDC_HOSTED 0
#define WC_USE_LIBC_STRING 0
#define WC_USE_LIBC_QSORT 0
#define WC_HAVE_ERRNO 0
#define WC_NO_HEAP 1
#include "wordcount.h"

void f(wc_limits *p)
{
    wc_limits_init(p);
}
]=])

set(common_flags
  -std=c99
  -O0
  -ffreestanding
  -fno-builtin
  -I "${WC_SOURCE_DIR}"
  -DWC_NO_HEAP=1
  -DWC_STDC_HOSTED=0
  -DWC_USE_LIBC_STRING=0
  -DWC_USE_LIBC_QSORT=0
  -DWC_HAVE_ERRNO=0
)

function(wc_check_no_libc_symbols name source)
  set(obj "${WC_TMP_DIR}/${name}.o")
  execute_process(
    COMMAND "${WC_C_COMPILER}" ${common_flags} -c "${source}" -o "${obj}"
    RESULT_VARIABLE compile_rc
    OUTPUT_VARIABLE compile_out
    ERROR_VARIABLE compile_err
  )
  if(NOT compile_rc EQUAL 0)
    message(FATAL_ERROR
      "compile failed for ${name}\n${compile_out}\n${compile_err}")
  endif()

  execute_process(
    COMMAND "${WC_NM}" -u "${obj}"
    RESULT_VARIABLE nm_rc
    OUTPUT_VARIABLE nm_out
    ERROR_VARIABLE nm_err
  )
  if(NOT nm_rc EQUAL 0)
    message(FATAL_ERROR "nm failed for ${name}\n${nm_out}\n${nm_err}")
  endif()

  if(nm_out MATCHES "(^|[\r\n\t ])U[\t ]+(memset|memcpy)($|[\r\n])")
    message(FATAL_ERROR
      "${name} references libc memory routines in no-libc mode:\n${nm_out}")
  endif()
endfunction()

wc_check_no_libc_symbols(use_limits "${use_limits_src}")
wc_check_no_libc_symbols(wordcount "${WC_SOURCE_DIR}/wordcount.c")
