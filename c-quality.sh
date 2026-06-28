#!/usr/bin/env bash
set -euo pipefail

# Usage:
#   ./c-quality.sh [source_root] [build_dir_with_compile_commands]
#
# Examples after configuring a CMake build:
#   ./c-quality.sh .
#   ./c-quality.sh . build/clang

readonly JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 4)}"
readonly SRC_ROOT="$(cd "${1:-.}" && pwd)"
if [[ -n "${2:-}" ]]; then
  readonly BUILD_HINT="$(cd "$2" && pwd)"
else
  readonly BUILD_HINT=""
fi
readonly CDB="compile_commands.json"
cd "$SRC_ROOT"

note() { printf '\033[0;34m[INFO]\033[0m %s\n' "$*"; }
fail() { printf '\033[0;31m[FAIL]\033[0m %s\n' "$*" >&2; exit 1; }

detect_cppcheck_misra() {
  local tmp
  tmp="$(mktemp "${TMPDIR:-/tmp}/wc-cppcheck-misra.XXXXXX.c")" || return 1
  printf '/* cppcheck MISRA addon probe. */\n' >"$tmp" || {
    rm -f "$tmp"
    return 1
  }
  cppcheck --addon=misra --language=c --std=c99 --quiet "$tmp" >/dev/null 2>&1
  local rc=$?
  rm -f "$tmp"
  return "$rc"
}

for tool in clang-format clangd; do
  command -v "$tool" >/dev/null 2>&1 || fail "Missing tool: $tool"
done
HAS_CPPCHECK=0
HAS_CPPCHECK_MISRA=0
if command -v cppcheck >/dev/null 2>&1; then
  HAS_CPPCHECK=1
  if detect_cppcheck_misra; then
    HAS_CPPCHECK_MISRA=1
  fi
else
  note "Optional cppcheck not found; skipping cppcheck checks."
fi

# Prefer repository sources, including new untracked files, while still
# excluding ignored build artifacts.
list_files() {
  if command -v git >/dev/null 2>&1 && git -C "$SRC_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git -C "$SRC_ROOT" ls-files -z --cached --others --exclude-standard -- '*.c' '*.h'
  else
    find "$SRC_ROOT" -type d \( -name .git -o -name build -o -name 'build-*' \) -prune \
      -o -type f \( -name '*.c' -o -name '*.h' \) -print0
  fi
}

list_c_files() {
  if command -v git >/dev/null 2>&1 && git -C "$SRC_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git -C "$SRC_ROOT" ls-files -z --cached --others --exclude-standard -- '*.c'
  else
    find "$SRC_ROOT" -type d \( -name .git -o -name build -o -name 'build-*' \) -prune \
      -o -type f -name '*.c' -print0
  fi
}

list_cpp_files() {
  if command -v git >/dev/null 2>&1 && git -C "$SRC_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git -C "$SRC_ROOT" ls-files -z --cached --others --exclude-standard -- '*.cpp' '*.cxx' '*.cc'
  else
    find "$SRC_ROOT" -type d \( -name .git -o -name build -o -name 'build-*' \) -prune \
      -o -type f \( -name '*.cpp' -o -name '*.cxx' -o -name '*.cc' \) -print0
  fi
}

detect_cdb_dir() {
  # 1) explicit build hint
  if [[ -n "$BUILD_HINT" && -f "$BUILD_HINT/$CDB" ]]; then
    printf '%s\n' "$BUILD_HINT"
    return 0
  fi

  # 2) repo root
  if [[ -f "$SRC_ROOT/$CDB" ]]; then
    printf '%s\n' "$SRC_ROOT"
    return 0
  fi

  # 3) common build dir
  if [[ -f "$SRC_ROOT/build/$CDB" ]]; then
    printf '%s\n' "$SRC_ROOT/build"
    return 0
  fi

  # 4) common preset build dirs
  if [[ -f "$SRC_ROOT/build/clang/$CDB" ]]; then
    printf '%s\n' "$SRC_ROOT/build/clang"
    return 0
  fi
  if [[ -f "$SRC_ROOT/build/gcc/$CDB" ]]; then
    printf '%s\n' "$SRC_ROOT/build/gcc"
    return 0
  fi

  return 1
}

compiler_kind() {
  local -a argv=("$@")
  if "${argv[@]}" --version 2>/dev/null | grep -qi clang; then
    printf '%s\n' clang
  elif "${argv[@]}" --version 2>/dev/null | grep -Eqi 'gcc|g\+\+|gnu'; then
    printf '%s\n' gcc
  else
    printf '%s\n' other
  fi
}

compiler_identity_key() {
  local kind="$1"
  shift
  local -a argv=("$@")
  local target
  local version

  target="$("${argv[@]}" -dumpmachine 2>/dev/null | head -n 1 || true)"
  version="$("${argv[@]}" -dumpfullversion -dumpversion 2>/dev/null | head -n 1 || true)"
  if [[ -z "$version" ]]; then
    version="$("${argv[@]}" --version 2>/dev/null | head -n 1 || true)"
  fi
  printf '%s|%s|%s\n' "$kind" "$target" "$version"
}

STRICT_FLAGS=()
STRICT_C_RUNS=0
STRICT_CXX_RUNS=0

strict_c_flags_for() {
  local kind="$1"
  STRICT_FLAGS=(
    -std=c99
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    -Wcast-align
    -Wcast-qual
    -Wstrict-prototypes
    -Wold-style-definition
    -Wmissing-prototypes
    -Wmissing-declarations
    -Wredundant-decls
    -Wnested-externs
    -Wpointer-arith
    -Wwrite-strings
    -Wformat=2
    -Winit-self
    -Wundef
    -Wbad-function-cast
    -Wswitch-enum
    -Wfloat-equal
    -Walloca
    -Wvla
    -Wdate-time
    -Werror
    -I"$SRC_ROOT"
    -fsyntax-only
  )
  case "$kind" in
    gcc)
      STRICT_FLAGS+=(
        -Wcast-align=strict
        -Wduplicated-branches
        -Wduplicated-cond
        -Wlogical-op
        -Wstrict-overflow=5
      )
      ;;
    clang)
      STRICT_FLAGS+=(
        -Wassign-enum
        -Wcovered-switch-default
        -Wextra-semi
        -Wmissing-variable-declarations
        -Wnewline-eof
        -Wshorten-64-to-32
        -Wunreachable-code-aggressive
      )
      ;;
  esac
}

strict_cxx_flags_for() {
  local kind="$1"
  STRICT_FLAGS=(
    -std=c++11
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    -Wcast-align
    -Wcast-qual
    -Wold-style-cast
    -Wmissing-declarations
    -Wredundant-decls
    -Wpointer-arith
    -Wwrite-strings
    -Wformat=2
    -Winit-self
    -Wundef
    -Wswitch-enum
    -Wfloat-equal
    -Walloca
    -Wvla
    -Wdate-time
    -Wzero-as-null-pointer-constant
    -Werror
    -I"$SRC_ROOT"
    -fsyntax-only
  )
  case "$kind" in
    gcc)
      STRICT_FLAGS+=(
        -Wcast-align=strict
        -Wduplicated-branches
        -Wduplicated-cond
        -Wlogical-op
        -Wstrict-overflow=5
        -Wuseless-cast
      )
      ;;
    clang)
      STRICT_FLAGS+=(
        -Wassign-enum
        -Wcovered-switch-default
        -Wextra-semi
        -Wmissing-variable-declarations
        -Wnewline-eof
        -Wshorten-64-to-32
        -Wunreachable-code-aggressive
      )
      ;;
  esac
}

run_strict_c_syntax() {
  local label="$1"
  shift
  local -a cc_argv=("$@")
  local kind
  local -a c_files=()

  kind="$(compiler_kind "${cc_argv[@]}")"
  if [[ "$kind" == other ]]; then
    note "Unsupported compiler for strict $label C syntax checks; skipping: ${cc_argv[*]}"
    return 0
  fi
  strict_c_flags_for "$kind"

  while IFS= read -r -d '' f; do
    c_files+=("$f")
  done < <(list_c_files)

  if ((${#c_files[@]} == 0)); then
    note "No C sources found; skipping strict $label syntax checks."
    return 0
  fi

  note "Running strict $label C syntax checks..."
  for source in "${c_files[@]}"; do
    note "  $label: $source"
    "${cc_argv[@]}" "${STRICT_FLAGS[@]}" "$source"
  done
  STRICT_C_RUNS=$((STRICT_C_RUNS + 1))
}

run_strict_cxx_syntax() {
  local label="$1"
  shift
  local -a cxx_argv=("$@")
  local kind
  local -a cpp_files=()

  kind="$(compiler_kind "${cxx_argv[@]}")"
  if [[ "$kind" == other ]]; then
    note "Unsupported compiler for strict $label C++ syntax checks; skipping: ${cxx_argv[*]}"
    return 0
  fi
  strict_cxx_flags_for "$kind"

  while IFS= read -r -d '' f; do
    cpp_files+=("$f")
  done < <(list_cpp_files)

  if ((${#cpp_files[@]} == 0)); then
    note "No C++ sources found; skipping strict $label syntax checks."
    return 0
  fi

  note "Running strict $label C++ syntax checks..."
  for source in "${cpp_files[@]}"; do
    note "  $label: $source"
    "${cxx_argv[@]}" "${STRICT_FLAGS[@]}" "$source"
  done
  STRICT_CXX_RUNS=$((STRICT_CXX_RUNS + 1))
}

fail_if_strict_c_uncovered() {
  local found=0
  local f

  while IFS= read -r -d '' f; do
    found=1
    break
  done < <(list_c_files)
  if ((found && STRICT_C_RUNS == 0)); then
    fail "No supported GCC/Clang-compatible C compiler ran strict syntax checks."
  fi
}

fail_if_strict_cxx_uncovered() {
  local found=0
  local f

  while IFS= read -r -d '' f; do
    found=1
    break
  done < <(list_cpp_files)
  if ((found && STRICT_CXX_RUNS == 0)); then
    fail "No supported GCC/Clang-compatible C++ compiler ran strict syntax checks."
  fi
}

STRICT_C_COMPILER_KEYS=()
STRICT_C_COMPILER_LABELS=()

strict_seen_label() {
  local key="$1"
  local i
  for ((i = 0; i < ${#STRICT_C_COMPILER_KEYS[@]}; i++)); do
    if [[ "${STRICT_C_COMPILER_KEYS[$i]}" == "$key" ]]; then
      printf '%s\n' "${STRICT_C_COMPILER_LABELS[$i]}"
      return 0
    fi
  done
  return 1
}

strict_remember_c_compiler() {
  STRICT_C_COMPILER_KEYS+=("$1")
  STRICT_C_COMPILER_LABELS+=("$2")
}

run_strict_c_compiler_once() {
  local label="$1"
  shift
  local -a argv=("$@")
  local bin
  local kind
  local key
  local seen_label

  command -v "${argv[0]}" >/dev/null 2>&1 || fail "Missing C compiler: ${argv[0]}"
  bin="$(command -v "${argv[0]}")"
  kind="$(compiler_kind "${argv[@]}")"
  if [[ "$kind" == other ]]; then
    key="$(readlink -f "$bin" 2>/dev/null || printf '%s\n' "$bin")"
  else
    key="$(compiler_identity_key "$kind" "${argv[@]}")"
  fi
  if seen_label="$(strict_seen_label "$key")"; then
    note "Strict $label C syntax checks already covered by $seen_label; skipping duplicate compiler."
    return 0
  fi
  strict_remember_c_compiler "$key" "$label"
  run_strict_c_syntax "$label" "${argv[@]}"
}

STRICT_CXX_COMPILER_KEYS=()
STRICT_CXX_COMPILER_LABELS=()

strict_seen_cxx_label() {
  local key="$1"
  local i
  for ((i = 0; i < ${#STRICT_CXX_COMPILER_KEYS[@]}; i++)); do
    if [[ "${STRICT_CXX_COMPILER_KEYS[$i]}" == "$key" ]]; then
      printf '%s\n' "${STRICT_CXX_COMPILER_LABELS[$i]}"
      return 0
    fi
  done
  return 1
}

strict_remember_cxx_compiler() {
  STRICT_CXX_COMPILER_KEYS+=("$1")
  STRICT_CXX_COMPILER_LABELS+=("$2")
}

run_strict_cxx_compiler_once() {
  local label="$1"
  shift
  local -a argv=("$@")
  local bin
  local kind
  local key
  local seen_label

  if ! command -v "${argv[0]}" >/dev/null 2>&1; then
    note "C++ compiler not found for strict $label syntax checks: ${argv[0]}"
    return 0
  fi
  bin="$(command -v "${argv[0]}")"
  kind="$(compiler_kind "${argv[@]}")"
  if [[ "$kind" == other ]]; then
    key="$(readlink -f "$bin" 2>/dev/null || printf '%s\n' "$bin")"
  else
    key="$(compiler_identity_key "$kind" "${argv[@]}")"
  fi
  if seen_label="$(strict_seen_cxx_label "$key")"; then
    note "Strict $label C++ syntax checks already covered by $seen_label; skipping duplicate compiler."
    return 0
  fi
  strict_remember_cxx_compiler "$key" "$label"
  run_strict_cxx_syntax "$label" "${argv[@]}"
}

note "Tool versions:"
note "  clang-format: $(clang-format --version)"
note "  clangd:       $(clangd --version | head -n 1)"
if ((HAS_CPPCHECK)); then
  note "  cppcheck:     $(cppcheck --version)"
  if ((HAS_CPPCHECK_MISRA)); then
    note "  cppcheck MISRA addon: available"
  else
    note "  cppcheck MISRA addon: not available"
  fi
fi

note "Checking clang-format..."
files=()
while IFS= read -r -d '' f; do
  files+=("$f")
done < <(list_files)
if ((${#files[@]} == 0)); then
  note "No source files found; skipping clang-format."
else
  printf '%s\0' "${files[@]}" | xargs -0 -P "$JOBS" clang-format --dry-run --Werror
fi

# CC/CXX are whitespace-split command words. Use a wrapper script for paths or
# arguments that require shell quoting.
cc_cmd="${CC:-cc}"
read -r -a cc_argv <<< "$cc_cmd"
run_strict_c_compiler_once "configured CC" "${cc_argv[@]}"
if command -v gcc >/dev/null 2>&1; then
  run_strict_c_compiler_once "gcc" gcc
fi
if command -v clang >/dev/null 2>&1; then
  run_strict_c_compiler_once "clang" clang
fi

cxx_cmd="${CXX:-c++}"
read -r -a cxx_argv <<< "$cxx_cmd"
run_strict_cxx_compiler_once "configured CXX" "${cxx_argv[@]}"
if command -v g++ >/dev/null 2>&1; then
  run_strict_cxx_compiler_once "g++" g++
fi
if command -v clang++ >/dev/null 2>&1; then
  run_strict_cxx_compiler_once "clang++" clang++
fi
fail_if_strict_c_uncovered
fail_if_strict_cxx_uncovered

note "Running clangd semantic checks..."
if cdb_dir="$(detect_cdb_dir)"; then
  c_files=()
  while IFS= read -r -d '' f; do
    c_files+=("$f")
  done < <(list_c_files)
  if ((${#c_files[@]} == 0)); then
    note "No C sources found; skipping clangd."
  else
    for source in "${c_files[@]}"; do
      if grep -Eq '^[[:space:]]*#[[:space:]]*include[[:space:]]+"(\.\./)?wordcount\.c"' "$source"; then
        note "  clangd: skipping white-box implementation include: $source"
        continue
      fi
      note "  clangd: $source"
      clangd --background-index=false --clang-tidy --enable-config --log=error \
        --compile-commands-dir="$cdb_dir" --check="$source"
    done
  fi
else
  fail "No $CDB found (expected in explicit build hint, repo root, build, build/clang, or build/gcc); cannot run required clangd checks."
fi

if ((HAS_CPPCHECK)); then
  note "Running cppcheck (hard fail on warnings/perf/portability)..."
  cc_cmd="${CC:-cc}"
  read -r -a cc_argv <<< "$cc_cmd"
  cc_bin="${cc_argv[0]}"

  ptrdiff_max=""

  if command -v "$cc_bin" >/dev/null 2>&1; then
    # Prefer compiler built-in (works without headers on GCC/Clang).
    ptrdiff_max="$("${cc_argv[@]}" -dM -E - </dev/null 2>/dev/null \
      | awk '/^#define __PTRDIFF_MAX__ / { $1=""; $2=""; sub(/^  */, ""); print; exit }')"

    # Fallback: force PTRDIFF_MAX by including <stdint.h>.
    if [[ -z "$ptrdiff_max" ]]; then
      ptrdiff_max="$(printf '#include <stdint.h>\n' \
        | "${cc_argv[@]}" -dM -E -xc - 2>/dev/null \
        | awk '/^#define PTRDIFF_MAX / { $1=""; $2=""; sub(/^  */, ""); print; exit }')"
    fi
  fi

  cppcheck_defines=()
  if [[ -n "$ptrdiff_max" ]]; then
    cppcheck_defines+=("-DWC_PTRDIFF_MAX=$ptrdiff_max")
  else
    fail "Could not determine PTRDIFF_MAX (no compiler, and no usable macro). Set WC_PTRDIFF_MAX manually for cppcheck."
  fi
  hard_args=(
    --check-level=exhaustive
    --enable=warning,performance,portability
    --inconclusive --quiet --inline-suppr
    --suppress=missingIncludeSystem
    --suppress=unmatchedSuppression
    --error-exitcode=1 -j "$JOBS"
  )

  if cdb_dir="$(detect_cdb_dir)"; then
    cppcheck "${hard_args[@]}" "${cppcheck_defines[@]}" --project="$cdb_dir/$CDB"
  else
    files=()
    while IFS= read -r -d '' f; do
      files+=("$f")
    done < <(list_files)
    if ((${#files[@]} == 0)); then
      note "No source files found; skipping cppcheck (hard)."
    else
      printf '%s\0' "${files[@]}" | xargs -0 cppcheck "${hard_args[@]}" "${cppcheck_defines[@]}" --language=c --std=c99 -I"$SRC_ROOT"
    fi
  fi

  if ((HAS_CPPCHECK_MISRA)); then
    note "Running cppcheck MISRA addon (actual rule diagnostics are hard failures)..."
    misra_args=(
      --addon=misra
      --suppress=missingIncludeSystem
      --suppress=unmatchedSuppression
      # cppcheck's MISRA addon emits misra-config for project macros such as
      # offsetof/member probes even when the normal parser has enough context.
      # Keep the gate focused on actionable MISRA rule diagnostics.
      --suppress=misra-config
      --error-exitcode=1
      --quiet
    )

    if cdb_dir="$(detect_cdb_dir)"; then
      cppcheck "${misra_args[@]}" "${cppcheck_defines[@]}" --file-filter='*.c' --project="$cdb_dir/$CDB"
    else
      c_files=()
      while IFS= read -r -d '' f; do
        c_files+=("$f")
      done < <(list_c_files)
      if ((${#c_files[@]} == 0)); then
        note "No C sources found; skipping cppcheck MISRA addon."
      else
        printf '%s\0' "${c_files[@]}" \
          | xargs -0 cppcheck "${misra_args[@]}" "${cppcheck_defines[@]}" --language=c --std=c99 -I"$SRC_ROOT"
      fi
    fi
  else
    note "Optional cppcheck MISRA addon not found; skipping MISRA checks."
  fi

  note "Running cppcheck (style, informational only)..."
  soft_args=(
    --check-level=exhaustive
    --enable=style
    --inconclusive --quiet --inline-suppr
    --suppress=missingIncludeSystem
    --suppress=unmatchedSuppression
    # Public API headers will *always* look unused to cppcheck inside the library itself.
    --suppress=unusedStructMember
    -j "$JOBS"
  )

  if cdb_dir="$(detect_cdb_dir)"; then
    cppcheck "${soft_args[@]}" "${cppcheck_defines[@]}" --project="$cdb_dir/$CDB" || true
  else
    files=()
    while IFS= read -r -d '' f; do
      files+=("$f")
    done < <(list_files)
    if ((${#files[@]} == 0)); then
      note "No source files found; skipping cppcheck (style)."
    else
      printf '%s\0' "${files[@]}" | xargs -0 cppcheck "${soft_args[@]}" "${cppcheck_defines[@]}" --language=c --std=c99 -I"$SRC_ROOT" || true
    fi
  fi
fi

if ((HAS_CPPCHECK_MISRA)); then
  note "All required quality checks passed (cppcheck hard checks and MISRA addon included)."
elif ((HAS_CPPCHECK)); then
  note "All required quality checks passed (cppcheck hard checks included)."
else
  note "All required quality checks passed (optional cppcheck skipped)."
fi
