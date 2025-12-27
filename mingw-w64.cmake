# ----------------------------------------------
# mingw-w64.cmake – cross-compile for Windows
# ----------------------------------------------
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Toolchain triplet (no trailing dash)
set(TOOLCHAIN_TRIPLET "x86_64-w64-mingw32")

# Compilers
set(CMAKE_C_COMPILER   "${TOOLCHAIN_TRIPLET}-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_TRIPLET}-g++")
set(CMAKE_RC_COMPILER  "${TOOLCHAIN_TRIPLET}-windres")

# Root for finding headers/libs
set(CMAKE_FIND_ROOT_PATH "/usr/${TOOLCHAIN_TRIPLET}")

# Don't try to run executables during try_compile for cross builds
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
