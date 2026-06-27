cmake_minimum_required(VERSION 3.20)
project(wordcount VERSION 5.0.0 LANGUAGES C)

include(GNUInstallDirs)

# --- Options ---
option(ENABLE_SANITIZERS "Enable Address+UB sanitizers (GCC/Clang only)" OFF)
option(WC_WERROR "Treat compiler warnings as errors when supported" OFF)
option(WC_MINGW_STATIC "Static link MinGW executables (helps running under Wine)" ON)
option(WC_BUILD_CLI "Build the wc CLI tool" ON)
option(WC_INSTALL "Install library + CMake package config" OFF)
option(WC_BUILD_SHARED "Build shared libraries (in addition to static)" OFF)
option(WC_INSTALL_PKGCONFIG "Install pkg-config metadata when WC_INSTALL is ON" ON)
option(WC_HASH_STRONG "Use keyed SipHash-2-4 instead of FNV-1a" ON)
option(WC_USE_LIBC_STRING "Use libc string/memory functions" ON)
option(WC_ENABLE_VALIDATE "Compile wc_validate() invariant checks" OFF)
option(WC_BUILD_VARIANTS "Build heap/tiny library variants (same symbols; do not link together)" OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Standard, project-wide defaults (still allow per-target overrides)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(CTest) # provides BUILD_TESTING and enables testing when ON

if(BUILD_TESTING)
  set(WC_BUILD_VARIANTS ON)
endif()

# Detect CompCert by compiler executable name (CMake may not have a built-in ID)
get_filename_component(WC_C_COMPILER_BASENAME "${CMAKE_C_COMPILER}" NAME)
set(WC_COMPILER_IS_COMPCERT OFF)
if(WC_C_COMPILER_BASENAME MATCHES "^(ccomp|ccomp\\.exe)$")
  set(WC_COMPILER_IS_COMPCERT ON)
endif()

function(wc_apply_common target)
  get_target_property(_type ${target} TYPE)
  set(_is_interface OFF)
  if(_type STREQUAL "INTERFACE_LIBRARY")
    set(_is_interface ON)
  endif()

  if(_is_interface)
    target_include_directories(${target} INTERFACE
      $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/wordcount>
    )
    set(_opt_scope INTERFACE)
    set(_def_scope INTERFACE)
  elseif(_type STREQUAL "STATIC_LIBRARY" OR _type STREQUAL "SHARED_LIBRARY" OR
         _type STREQUAL "MODULE_LIBRARY" OR _type STREQUAL "OBJECT_LIBRARY")
    target_include_directories(${target} PUBLIC
      $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/wordcount>
    )
    set(_opt_scope PRIVATE)
    set(_def_scope PUBLIC)
  else()
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}")
    set(_opt_scope PRIVATE)
    set(_def_scope PRIVATE)
  endif()

  # Warnings: keep strict for GCC/Clang, conservative for CompCert, MSVC-friendly
  if(MSVC)
    # /permissive- is primarily a C++ conformance switch; this is a C-only project.
    target_compile_options(${target} ${_opt_scope} /W4)
    if(WC_WERROR)
      target_compile_options(${target} ${_opt_scope} /WX)
    endif()
  elseif(WC_COMPILER_IS_COMPCERT)
    # CompCert supports -Wall and -W<warning>/ -Werror (but not all GCC warnings)
    target_compile_options(${target} ${_opt_scope} -Wall)
    if(WC_WERROR)
      target_compile_options(${target} ${_opt_scope} -Werror)
    endif()
  else()
    target_compile_options(${target} ${_opt_scope}
      "$<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>"
      "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>"
    )
    if(WC_WERROR)
      target_compile_options(${target} ${_opt_scope} -Werror)
    endif()
  endif()

  foreach(_wc_def HASH_STRONG USE_LIBC_STRING ENABLE_VALIDATE)
    get_target_property(_wc_${_wc_def} ${target} WC_DEF_${_wc_def})
    if(_wc_${_wc_def} STREQUAL "_wc_${_wc_def}-NOTFOUND")
      set(_wc_${_wc_def} "$<IF:$<BOOL:${WC_${_wc_def}}>,1,0>")
    endif()
  endforeach()

  target_compile_definitions(${target} ${_def_scope}
    WC_HASH_STRONG=${_wc_HASH_STRONG}
    WC_USE_LIBC_STRING=${_wc_USE_LIBC_STRING}
    WC_ENABLE_VALIDATE=${_wc_ENABLE_VALIDATE}
  )

  # Visibility: default to hidden for shared libs, export via WC_API
  if(_type STREQUAL "SHARED_LIBRARY")
    set_target_properties(${target} PROPERTIES
      C_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN YES
    )
  endif()

  # Sanitizers: only on native GCC/Clang (not MinGW, not MSVC, not CompCert)
  if(ENABLE_SANITIZERS AND NOT _is_interface)
    if(NOT MSVC AND NOT MINGW AND NOT WC_COMPILER_IS_COMPCERT)
      target_compile_options(${target} PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
      )
      get_target_property(_type ${target} TYPE)
      if(_type STREQUAL "EXECUTABLE" OR _type STREQUAL "SHARED_LIBRARY" OR _type STREQUAL "MODULE_LIBRARY")
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
      endif()
    endif()
  endif()

  # MinGW static linking (executables only)
  if(WIN32 AND NOT MSVC AND WC_MINGW_STATIC)
    get_target_property(_type ${target} TYPE)
    if(_type STREQUAL "EXECUTABLE")
      target_link_options(${target} PRIVATE -static)
    endif()
  endif()
endfunction()

set(WC_BUILD_SHARED_LIB OFF)
if(WC_BUILD_SHARED OR BUILD_SHARED_LIBS)
  set(WC_BUILD_SHARED_LIB ON)
endif()

# =============================================================================
# Core library variants
# =============================================================================

add_library(wordcount_lib STATIC wordcount.c)
wc_apply_common(wordcount_lib)
set_target_properties(wordcount_lib PROPERTIES
  OUTPUT_NAME wordcount
  EXPORT_NAME wordcount
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
)
add_library(wordcount::wordcount ALIAS wordcount_lib)

if(WC_BUILD_SHARED_LIB)
  add_library(wordcount_lib_shared SHARED wordcount.c)
  wc_apply_common(wordcount_lib_shared)
  set_target_properties(wordcount_lib_shared PROPERTIES
    OUTPUT_NAME wordcount_shared
    EXPORT_NAME wordcount_shared
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
  )
  target_compile_definitions(wordcount_lib_shared PRIVATE WC_BUILD_SHARED)
  target_compile_definitions(wordcount_lib_shared PUBLIC WC_SHARED)
  add_library(wordcount::wordcount_shared ALIAS wordcount_lib_shared)
endif()

if(WC_BUILD_VARIANTS)
  add_library(wordcount_lib_heap STATIC wordcount.c)
  target_compile_definitions(wordcount_lib_heap PUBLIC WC_STACK_BUFFER=0)
  wc_apply_common(wordcount_lib_heap)
  set_target_properties(wordcount_lib_heap PROPERTIES
    OUTPUT_NAME wordcount_heap
    EXPORT_NAME wordcount_heap
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
  )
  add_library(wordcount::wordcount_heap ALIAS wordcount_lib_heap)

  if(WC_BUILD_SHARED_LIB)
    add_library(wordcount_lib_heap_shared SHARED wordcount.c)
    target_compile_definitions(wordcount_lib_heap_shared PUBLIC WC_STACK_BUFFER=0)
    wc_apply_common(wordcount_lib_heap_shared)
    set_target_properties(wordcount_lib_heap_shared PROPERTIES
      OUTPUT_NAME wordcount_heap_shared
      EXPORT_NAME wordcount_heap_shared
      VERSION ${PROJECT_VERSION}
      SOVERSION ${PROJECT_VERSION_MAJOR}
    )
    target_compile_definitions(wordcount_lib_heap_shared PRIVATE WC_BUILD_SHARED)
    target_compile_definitions(wordcount_lib_heap_shared PUBLIC WC_SHARED)
    add_library(wordcount::wordcount_heap_shared ALIAS wordcount_lib_heap_shared)
  endif()

  add_library(wordcount_lib_tiny STATIC wordcount.c)
  target_compile_definitions(wordcount_lib_tiny PUBLIC
    WC_STACK_BUFFER=0
    WC_MIN_INIT_CAP=8
    WC_MIN_BLOCK_SZ=64
    WC_MAX_WORD=32
    WC_EXPECT_AUTOTUNE_RETRY=0
  )
  wc_apply_common(wordcount_lib_tiny)
  set_target_properties(wordcount_lib_tiny PROPERTIES
    OUTPUT_NAME wordcount_tiny
    EXPORT_NAME wordcount_tiny
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
  )
  add_library(wordcount::wordcount_tiny ALIAS wordcount_lib_tiny)

  if(WC_BUILD_SHARED_LIB)
    add_library(wordcount_lib_tiny_shared SHARED wordcount.c)
    target_compile_definitions(wordcount_lib_tiny_shared PUBLIC
      WC_STACK_BUFFER=0
      WC_MIN_INIT_CAP=8
      WC_MIN_BLOCK_SZ=64
      WC_MAX_WORD=32
    )
    wc_apply_common(wordcount_lib_tiny_shared)
    set_target_properties(wordcount_lib_tiny_shared PROPERTIES
      OUTPUT_NAME wordcount_tiny_shared
      EXPORT_NAME wordcount_tiny_shared
      VERSION ${PROJECT_VERSION}
      SOVERSION ${PROJECT_VERSION_MAJOR}
    )
    target_compile_definitions(wordcount_lib_tiny_shared PRIVATE WC_BUILD_SHARED)
    target_compile_definitions(wordcount_lib_tiny_shared PUBLIC WC_SHARED)
    add_library(wordcount::wordcount_tiny_shared ALIAS wordcount_lib_tiny_shared)
  endif()
endif()

# =============================================================================
# CLI
# =============================================================================

if(WC_BUILD_CLI)
  add_executable(wc wc_main.c)
  target_link_libraries(wc PRIVATE wordcount_lib)
  wc_apply_common(wc)
  if(WIN32)
    target_link_libraries(wc PRIVATE shell32)
  endif()
endif()

# =============================================================================
# Tests
# =============================================================================

if(BUILD_TESTING)
  include(CheckLanguage)
  check_language(CXX)
  if(CMAKE_CXX_COMPILER)
    enable_language(CXX)
  endif()

  function(create_test_variant name lib)
    add_executable(${name} wc_test.c)
    target_link_libraries(${name} PRIVATE ${lib})
    foreach(_wc_def HASH_STRONG USE_LIBC_STRING ENABLE_VALIDATE)
      get_target_property(_wc_${_wc_def} ${lib} WC_DEF_${_wc_def})
      if(NOT _wc_${_wc_def} STREQUAL "_wc_${_wc_def}-NOTFOUND")
        set_target_properties(${name} PROPERTIES
          WC_DEF_${_wc_def} "${_wc_${_wc_def}}")
      endif()
    endforeach()
    wc_apply_common(${name})
    add_test(NAME ${name} COMMAND ${name})
  endfunction()

  create_test_variant(wc_test      wordcount_lib)
  if(TARGET wordcount_lib_heap)
    create_test_variant(wc_test_heap wordcount_lib_heap)
  endif()
  if(TARGET wordcount_lib_tiny)
    create_test_variant(wc_test_tiny wordcount_lib_tiny)
  endif()

  add_library(wordcount_lib_fnv STATIC wordcount.c)
  set_target_properties(wordcount_lib_fnv PROPERTIES WC_DEF_HASH_STRONG 0)
  wc_apply_common(wordcount_lib_fnv)
  create_test_variant(wc_test_fnv wordcount_lib_fnv)

  add_library(wordcount_lib_full_table_tiny STATIC wordcount.c)
  target_compile_definitions(wordcount_lib_full_table_tiny PUBLIC
    WC_STACK_BUFFER=0
    WC_MIN_INIT_CAP=1
    WC_DEFAULT_INIT_CAP=1
    WC_EXPECT_AUTOTUNE_RETRY=0
    WC_MIN_BLOCK_SZ=64
    WC_DEFAULT_BLOCK_SZ=256
    WC_MAX_WORD=32
  )
  wc_apply_common(wordcount_lib_full_table_tiny)
  set_target_properties(wordcount_lib_full_table_tiny PROPERTIES
    OUTPUT_NAME wordcount_full_table_tiny
    EXPORT_NAME wordcount_full_table_tiny
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
  )
  add_executable(wc_test_full_table_tiny tests/full_table_tiny.c)
  target_link_libraries(wc_test_full_table_tiny PRIVATE
    wordcount_lib_full_table_tiny)
  wc_apply_common(wc_test_full_table_tiny)
  add_test(NAME wc_test_full_table_tiny COMMAND wc_test_full_table_tiny)

  add_library(wordcount_lib_force_hash STATIC wordcount.c)
  target_compile_definitions(wordcount_lib_force_hash PRIVATE
    WC_INTERNAL_TESTING=1
    WC_INTERNAL_TEST_FORCE_HASH=1
  )
  wc_apply_common(wordcount_lib_force_hash)
  create_test_variant(wc_test_force_hash wordcount_lib_force_hash)

  add_library(wordcount_lib_noheap STATIC wordcount.c)
  target_compile_definitions(wordcount_lib_noheap PUBLIC
    WC_NO_HEAP=1
    WC_STACK_BUFFER=0
  )
  wc_apply_common(wordcount_lib_noheap)

  add_executable(wc_test_noheap tests/noheap_static.c)
  target_link_libraries(wc_test_noheap PRIVATE wordcount_lib_noheap)
  wc_apply_common(wc_test_noheap)
  add_test(NAME wc_test_noheap COMMAND wc_test_noheap)

  add_library(wordcount_lib_noheap_nouintptr STATIC wordcount.c)
  target_compile_definitions(wordcount_lib_noheap_nouintptr PUBLIC
    WC_NO_HEAP=1
    WC_STACK_BUFFER=0
    WC_HAVE_UINTPTR=0
    WC_TRUST_STATIC_BUFFER_ALIGNMENT=1
  )
  wc_apply_common(wordcount_lib_noheap_nouintptr)

  add_executable(wc_test_noheap_nouintptr tests/noheap_static.c)
  target_link_libraries(wc_test_noheap_nouintptr PRIVATE
    wordcount_lib_noheap_nouintptr)
  wc_apply_common(wc_test_noheap_nouintptr)
  add_test(NAME wc_test_noheap_nouintptr COMMAND wc_test_noheap_nouintptr)

  add_library(wordcount_lib_noheap_nonlinear_uintptr STATIC wordcount.c)
  target_compile_definitions(wordcount_lib_noheap_nonlinear_uintptr PUBLIC
    WC_NO_HEAP=1
    WC_STACK_BUFFER=0
    WC_HAVE_UINTPTR=1
    WC_LINEAR_UINTPTR_ALIGNMENT=0
  )
  wc_apply_common(wordcount_lib_noheap_nonlinear_uintptr)

  add_executable(wc_test_noheap_nonlinear_uintptr tests/noheap_static.c)
  target_link_libraries(wc_test_noheap_nonlinear_uintptr PRIVATE
    wordcount_lib_noheap_nonlinear_uintptr)
  wc_apply_common(wc_test_noheap_nonlinear_uintptr)
  add_test(NAME wc_test_noheap_nonlinear_uintptr
    COMMAND wc_test_noheap_nonlinear_uintptr)

  add_library(wordcount_lib_noheap_nonlinear_uintptr_trust STATIC wordcount.c)
  target_compile_definitions(wordcount_lib_noheap_nonlinear_uintptr_trust PUBLIC
    WC_NO_HEAP=1
    WC_STACK_BUFFER=0
    WC_HAVE_UINTPTR=1
    WC_LINEAR_UINTPTR_ALIGNMENT=0
    WC_TRUST_STATIC_BUFFER_ALIGNMENT=1
  )
  wc_apply_common(wordcount_lib_noheap_nonlinear_uintptr_trust)

  add_executable(wc_test_noheap_nonlinear_uintptr_trust tests/noheap_static.c)
  target_link_libraries(wc_test_noheap_nonlinear_uintptr_trust PRIVATE
    wordcount_lib_noheap_nonlinear_uintptr_trust)
  wc_apply_common(wc_test_noheap_nonlinear_uintptr_trust)
  add_test(NAME wc_test_noheap_nonlinear_uintptr_trust
    COMMAND wc_test_noheap_nonlinear_uintptr_trust)

  add_library(wordcount_lib_noheap_tiny_autotune STATIC wordcount.c)
  set_target_properties(wordcount_lib_noheap_tiny_autotune PROPERTIES
    WC_DEF_USE_LIBC_STRING 0
  )
  target_compile_definitions(wordcount_lib_noheap_tiny_autotune PUBLIC
    WC_NO_HEAP=1
    WC_STDC_HOSTED=0
    WC_USE_LIBC_QSORT=0
    WC_HAVE_ERRNO=0
    WC_MAX_WORD=4
    WC_MIN_INIT_CAP=1
    WC_MIN_BLOCK_SZ=1
    WC_DEFAULT_INIT_CAP=128
    WC_DEFAULT_BLOCK_SZ=256
  )
  wc_apply_common(wordcount_lib_noheap_tiny_autotune)

  add_executable(wc_test_noheap_tiny_autotune
    tests/noheap_tiny_autotune.c)
  target_link_libraries(wc_test_noheap_tiny_autotune PRIVATE
    wordcount_lib_noheap_tiny_autotune)
  set_target_properties(wc_test_noheap_tiny_autotune PROPERTIES
    WC_DEF_USE_LIBC_STRING 0
  )
  wc_apply_common(wc_test_noheap_tiny_autotune)
  add_test(NAME wc_test_noheap_tiny_autotune
    COMMAND wc_test_noheap_tiny_autotune)

  add_executable(wc_test_counter_overflow tests/counter_overflow.c)
  set_target_properties(wc_test_counter_overflow PROPERTIES
    WC_DEF_ENABLE_VALIDATE 1
  )
  wc_apply_common(wc_test_counter_overflow)
  add_test(NAME wc_test_counter_overflow COMMAND wc_test_counter_overflow)

  add_library(wordcount_lib_ptrdiff_caps STATIC wordcount.c)
  target_compile_definitions(wordcount_lib_ptrdiff_caps PUBLIC
    WC_PTRDIFF_MAX=4095
    WC_DEFAULT_INIT_CAP=16
    WC_DEFAULT_BLOCK_SZ=256
    WC_MAX_WORD=64
    WC_STACK_BUFFER=0
  )
  wc_apply_common(wordcount_lib_ptrdiff_caps)

  add_executable(wc_test_ptrdiff_caps tests/ptrdiff_caps.c)
  target_link_libraries(wc_test_ptrdiff_caps PRIVATE wordcount_lib_ptrdiff_caps)
  wc_apply_common(wc_test_ptrdiff_caps)
  add_test(NAME wc_test_ptrdiff_caps COMMAND wc_test_ptrdiff_caps)

  add_library(wordcount_lib_collision STATIC wordcount.c)
  target_compile_definitions(wordcount_lib_collision PRIVATE
    WC_INTERNAL_TESTING=1
    WC_INTERNAL_TEST_FORCE_HASH=1
  )
  wc_apply_common(wordcount_lib_collision)

  add_executable(wc_test_collision wc_test_collision.c)
  target_link_libraries(wc_test_collision PRIVATE wordcount_lib_collision)
  wc_apply_common(wc_test_collision)
  add_test(NAME wc_test_collision COMMAND wc_test_collision)

  # Portable fault-injection (malloc failures at every site)
  add_library(wordcount_lib_fault STATIC wordcount.c)
  target_sources(wordcount_lib_fault PRIVATE wc_fault_alloc.c)
  target_compile_definitions(wordcount_lib_fault PUBLIC
    WC_MIN_BLOCK_SZ=1
    WC_DEFAULT_BLOCK_SZ=1
    WC_MALLOC=wc_fault_malloc
    WC_FREE=wc_fault_free
  )
  target_compile_options(wordcount_lib_fault PRIVATE
    "$<$<C_COMPILER_ID:MSVC>:/FI${PROJECT_SOURCE_DIR}/wc_fault_alloc.h>"
    "$<$<NOT:$<C_COMPILER_ID:MSVC>>:-include;${PROJECT_SOURCE_DIR}/wc_fault_alloc.h>"
  )
  wc_apply_common(wordcount_lib_fault)

  add_executable(wc_test_fault wc_test_fault.c)
  target_link_libraries(wc_test_fault PRIVATE wordcount_lib_fault)
  wc_apply_common(wc_test_fault)
  add_test(NAME wc_test_fault COMMAND wc_test_fault)

  add_library(wordcount_lib_fault_heap STATIC wordcount.c)
  target_sources(wordcount_lib_fault_heap PRIVATE wc_fault_alloc.c)
  target_compile_definitions(wordcount_lib_fault_heap PUBLIC
    WC_MIN_BLOCK_SZ=1
    WC_DEFAULT_BLOCK_SZ=1
    WC_STACK_BUFFER=0
    WC_MALLOC=wc_fault_malloc
    WC_FREE=wc_fault_free
  )
  target_compile_options(wordcount_lib_fault_heap PRIVATE
    "$<$<C_COMPILER_ID:MSVC>:/FI${PROJECT_SOURCE_DIR}/wc_fault_alloc.h>"
    "$<$<NOT:$<C_COMPILER_ID:MSVC>>:-include;${PROJECT_SOURCE_DIR}/wc_fault_alloc.h>"
  )
  wc_apply_common(wordcount_lib_fault_heap)

  add_executable(wc_test_fault_heap wc_test_fault.c)
  target_link_libraries(wc_test_fault_heap PRIVATE wordcount_lib_fault_heap)
  wc_apply_common(wc_test_fault_heap)
  add_test(NAME wc_test_fault_heap COMMAND wc_test_fault_heap)

  # Differential fuzzing target plus a short CTest smoke run. Only build under
  # Clang with libFuzzer available (skip on Apple toolchains where the runtime
  # may be absent in CI images).
  if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND NOT APPLE)
    add_executable(wc_fuzz_diff wc_fuzz_diff.c wordcount.c)
    wc_apply_common(wc_fuzz_diff)
    target_compile_options(wc_fuzz_diff PRIVATE -fsanitize=fuzzer-no-link)
    target_link_options(wc_fuzz_diff PRIVATE -fsanitize=fuzzer)
    add_test(NAME wc_fuzz_diff_smoke COMMAND wc_fuzz_diff -runs=1000)
  endif()

  function(wc_add_compile_only target src)
    add_library(${target} OBJECT ${src})
    wc_apply_common(${target})
    if(MSVC)
      target_compile_options(${target} PRIVATE /WX)
    else()
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endfunction()

  wc_add_compile_only(wc_compile_static_init tests/compile_static_init.c)
  wc_add_compile_only(wc_compile_msvc_sal tests/compile_msvc_sal.c)
  wc_add_compile_only(wc_compile_nouintptr wordcount.c)
  target_compile_definitions(wc_compile_nouintptr PRIVATE
    WC_HAVE_UINTPTR=0
    WC_TRUST_STATIC_BUFFER_ALIGNMENT=1
    WC_STACK_BUFFER=0
  )
  wc_add_compile_only(wc_compile_force_hash wordcount.c)
  target_compile_definitions(wc_compile_force_hash PRIVATE
    WC_INTERNAL_TESTING=1
    WC_INTERNAL_TEST_FORCE_HASH=1
  )
  wc_add_compile_only(wc_compile_custom_uintptr wordcount.c)
  target_compile_definitions(wc_compile_custom_uintptr PRIVATE
    WC_U32_T=unsigned
    WC_PTRDIFF_MAX=9223372036854775807L
    WC_HAVE_UINTPTR=1
  )
  wc_add_compile_only(wc_compile_auto_uintptr wordcount.c)
  target_compile_definitions(wc_compile_auto_uintptr PRIVATE
    WC_U32_T=unsigned
    WC_PTRDIFF_MAX=9223372036854775807L
  )
  wc_add_compile_only(wc_compile_nonlinear_uintptr wordcount.c)
  target_compile_definitions(wc_compile_nonlinear_uintptr PRIVATE
    WC_U32_T=unsigned
    WC_PTRDIFF_MAX=9223372036854775807L
    WC_HAVE_UINTPTR=1
    WC_LINEAR_UINTPTR_ALIGNMENT=0
    WC_STACK_BUFFER=0
  )
  add_library(wc_compile_noheap_libclight OBJECT wordcount.c)
  set_target_properties(wc_compile_noheap_libclight PROPERTIES
    WC_DEF_USE_LIBC_STRING 0
  )
  wc_apply_common(wc_compile_noheap_libclight)
  if(MSVC)
    target_compile_options(wc_compile_noheap_libclight PRIVATE /WX)
  else()
    target_compile_options(wc_compile_noheap_libclight PRIVATE -Werror)
  endif()
  target_compile_definitions(wc_compile_noheap_libclight PRIVATE
    WC_STDC_HOSTED=0
    WC_NO_HEAP=1
    WC_STACK_BUFFER=0
    WC_USE_LIBC_QSORT=0
    WC_HAVE_ERRNO=0
    WC_OMIT_ASSERT=1
  )
  add_library(wc_compile_noheap_libclight_nouintptr OBJECT wordcount.c)
  set_target_properties(wc_compile_noheap_libclight_nouintptr PROPERTIES
    WC_DEF_USE_LIBC_STRING 0
  )
  wc_apply_common(wc_compile_noheap_libclight_nouintptr)
  if(MSVC)
    target_compile_options(wc_compile_noheap_libclight_nouintptr PRIVATE /WX)
  else()
    target_compile_options(wc_compile_noheap_libclight_nouintptr PRIVATE -Werror)
  endif()
  target_compile_definitions(wc_compile_noheap_libclight_nouintptr PRIVATE
    WC_STDC_HOSTED=0
    WC_NO_HEAP=1
    WC_STACK_BUFFER=0
    WC_USE_LIBC_QSORT=0
    WC_HAVE_ERRNO=0
    WC_OMIT_ASSERT=1
    WC_HAVE_UINTPTR=0
  )

  if(CMAKE_CXX_COMPILER)
    wc_add_compile_only(wc_compile_cpp tests/compile_cpp.cpp)
    set_target_properties(wc_compile_cpp PROPERTIES
      CXX_STANDARD 11
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF
    )
  else()
    message(STATUS "C++ compiler not found; skipping C++ compile test.")
  endif()

  add_custom_target(wc_compile_checks ALL)
  add_dependencies(wc_compile_checks wc_compile_static_init)
  add_dependencies(wc_compile_checks wc_compile_msvc_sal)
  add_dependencies(wc_compile_checks wc_compile_nouintptr wc_compile_force_hash
                   wc_compile_custom_uintptr wc_compile_auto_uintptr
                   wc_compile_nonlinear_uintptr
                   wc_compile_noheap_libclight
                   wc_compile_noheap_libclight_nouintptr)
  if(TARGET wc_compile_cpp)
    add_dependencies(wc_compile_checks wc_compile_cpp)
  endif()

  add_test(NAME wc_compile_invalid_config
    COMMAND ${CMAKE_COMMAND}
      -DWC_C_COMPILER=${CMAKE_C_COMPILER}
      -DWC_C_COMPILER_ID=${CMAKE_C_COMPILER_ID}
      -DWC_SOURCE_DIR=${PROJECT_SOURCE_DIR}
      -DWC_TMP_DIR=${PROJECT_BINARY_DIR}/compile-invalid-config
      -P ${PROJECT_SOURCE_DIR}/tests/compile_invalid_config.cmake)

  if(CMAKE_NM AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    add_test(NAME wc_no_libc_symbols
      COMMAND ${CMAKE_COMMAND}
        -DWC_C_COMPILER=${CMAKE_C_COMPILER}
        -DWC_NM=${CMAKE_NM}
        -DWC_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DWC_TMP_DIR=${PROJECT_BINARY_DIR}/no-libc-symbols
        -P ${PROJECT_SOURCE_DIR}/tests/no_libc_symbols.cmake)
  endif()

  if(WC_BUILD_CLI AND UNIX)
    add_test(NAME wc_cli_behavior
      COMMAND ${CMAKE_COMMAND}
        -DWC_EXE=$<TARGET_FILE:wc>
        -DWC_TMP_DIR=${PROJECT_BINARY_DIR}/cli-behavior
        -P ${PROJECT_SOURCE_DIR}/tests/cli_behavior.cmake)
    add_test(NAME wc_cli_output_errors
      COMMAND ${CMAKE_COMMAND}
        -DWC_EXE=$<TARGET_FILE:wc>
        -DWC_TMP_DIR=${PROJECT_BINARY_DIR}/cli-output-errors
        -P ${PROJECT_SOURCE_DIR}/tests/cli_output_errors.cmake)
  endif()
endif()

if(WC_INSTALL)
  include(CMakePackageConfigHelpers)

  set(WC_INSTALL_TARGETS wordcount_lib)
  if(TARGET wordcount_lib_shared)
    list(APPEND WC_INSTALL_TARGETS wordcount_lib_shared)
  endif()
  if(TARGET wordcount_lib_heap)
    list(APPEND WC_INSTALL_TARGETS wordcount_lib_heap)
  endif()
  if(TARGET wordcount_lib_heap_shared)
    list(APPEND WC_INSTALL_TARGETS wordcount_lib_heap_shared)
  endif()
  if(TARGET wordcount_lib_tiny)
    list(APPEND WC_INSTALL_TARGETS wordcount_lib_tiny)
  endif()
  if(TARGET wordcount_lib_tiny_shared)
    list(APPEND WC_INSTALL_TARGETS wordcount_lib_tiny_shared)
  endif()

  install(TARGETS ${WC_INSTALL_TARGETS}
    EXPORT wordcountTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )

  install(FILES wordcount.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/wordcount)

  configure_package_config_file(
    ${PROJECT_SOURCE_DIR}/cmake/wordcountConfig.cmake.in
    ${PROJECT_BINARY_DIR}/wordcountConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
  )

  write_basic_package_version_file(
    ${PROJECT_BINARY_DIR}/wordcountConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
  )

  install(FILES
    ${PROJECT_BINARY_DIR}/wordcountConfig.cmake
    ${PROJECT_BINARY_DIR}/wordcountConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
  )

  install(EXPORT wordcountTargets
    NAMESPACE wordcount::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
  )

  install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/licenses/${PROJECT_NAME})

  if(WC_INSTALL_PKGCONFIG)
    if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}" OR
       IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
      set(WC_PKGCONFIG_PREFIX "${CMAKE_INSTALL_PREFIX}")
      set(WC_PKGCONFIG_LIBDIR "${CMAKE_INSTALL_FULL_LIBDIR}")
      set(WC_PKGCONFIG_INCLUDEDIR "${CMAKE_INSTALL_FULL_INCLUDEDIR}/wordcount")
    else()
      set(WC_PKGCONFIG_PREFIX "\${pcfiledir}")
      string(REPLACE "/" ";" _wc_pkgconfig_libdir_components
        "${CMAKE_INSTALL_LIBDIR}")
      foreach(_wc_pkgconfig_component IN LISTS _wc_pkgconfig_libdir_components)
        if(NOT _wc_pkgconfig_component STREQUAL "")
          set(WC_PKGCONFIG_PREFIX "${WC_PKGCONFIG_PREFIX}/..")
        endif()
      endforeach()
      set(WC_PKGCONFIG_PREFIX "${WC_PKGCONFIG_PREFIX}/..")
      set(WC_PKGCONFIG_LIBDIR "\${prefix}/${CMAKE_INSTALL_LIBDIR}")
      set(WC_PKGCONFIG_INCLUDEDIR
        "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}/wordcount")
    endif()
    configure_file(${PROJECT_SOURCE_DIR}/cmake/wordcount.pc.in
      ${PROJECT_BINARY_DIR}/wordcount.pc @ONLY)
    install(FILES ${PROJECT_BINARY_DIR}/wordcount.pc
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
    if(BUILD_TESTING AND NOT ENABLE_SANITIZERS)
      find_program(PKG_CONFIG_EXECUTABLE pkg-config)
      if(PKG_CONFIG_EXECUTABLE)
        add_test(NAME wc_pkgconfig_relocatable
          COMMAND ${CMAKE_COMMAND}
            -DWC_BUILD_DIR=${PROJECT_BINARY_DIR}
            -DWC_INSTALL_PREFIX=${PROJECT_BINARY_DIR}/pkgconfig-relocatable
            -DWC_PKGCONFIG_DIR=${CMAKE_INSTALL_LIBDIR}/pkgconfig
            -DWC_PKG_CONFIG=${PKG_CONFIG_EXECUTABLE}
            -DWC_C_COMPILER=${CMAKE_C_COMPILER}
            -P ${PROJECT_SOURCE_DIR}/tests/pkgconfig_relocatable.cmake)
      endif()
    endif()
  endif()
endif()
