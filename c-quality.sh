#!/usr/bin/env bash
set -euo pipefail

# Usage:
#   ./c-quality.sh [source_root] [build_dir_with_compile_commands]
#
# Examples:
#   ./c-quality.sh .
#   ./c-quality.sh . build

readonly JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 4)}"
readonly CLANG_TIDY_JOBS="${CLANG_TIDY_JOBS:-1}"
readonly SRC_ROOT="${1:-.}"
readonly BUILD_HINT="${2:-}"
readonly CDB="compile_commands.json"

note() { printf '\033[0;34m[INFO]\033[0m %s\n' "$*"; }
fail() { printf '\033[0;31m[FAIL]\033[0m %s\n' "$*" >&2; exit 1; }

for tool in clang-format clang-tidy cppcheck; do
  command -v "$tool" >/dev/null 2>&1 || fail "Missing tool: $tool"
done

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

detect_cdb_dir() {
  # 1) explicit build hint
  if [[ -n "$BUILD_HINT" && -f "$BUILD_HINT/$CDB" ]]; then
    printf '%s\n' "$BUILD_HINT"
    return 0
  fi

  # 2) repo root (your symlink case)
  if [[ -f "$SRC_ROOT/$CDB" ]]; then
    printf '%s\n' "$SRC_ROOT"
    return 0
  fi

  # 3) common build dir
  if [[ -f "$SRC_ROOT/build/$CDB" ]]; then
    printf '%s\n' "$SRC_ROOT/build"
    return 0
  fi

  return 1
}

note "Tool versions:"
note "  clang-format: $(clang-format --version)"
note "  clang-tidy:   $(clang-tidy --version)"
note "  cppcheck:     $(cppcheck --version)"

note "Running clang-format..."
files=()
while IFS= read -r -d '' f; do
  files+=("$f")
done < <(list_files)
if ((${#files[@]} == 0)); then
  note "No source files found; skipping clang-format."
else
  printf '%s\0' "${files[@]}" | xargs -0 -P "$JOBS" clang-format -i
fi

note "Running clang-tidy..."
c_files=()
while IFS= read -r -d '' f; do
  c_files+=("$f")
done < <(list_c_files)
if ((${#c_files[@]} == 0)); then
  note "No C sources found; skipping clang-tidy."
else
  # Use one deterministic C99 parse profile. The CMake database intentionally
  # contains many variant entries for wordcount.c; clang-tidy 18 can select a
  # duplicate variant that crashes before producing diagnostics.
  printf '%s\0' "${c_files[@]}" \
    | xargs -0 -P "$CLANG_TIDY_JOBS" -I{} clang-tidy --quiet "{}" -- \
      -std=c99 -I"$SRC_ROOT" -DWC_ENABLE_VALIDATE=1
fi

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

note "All quality checks passed (hard checks)."
