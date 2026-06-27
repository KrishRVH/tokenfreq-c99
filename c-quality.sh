#!/usr/bin/env bash
set -euo pipefail

# Usage:
#   ./c-quality.sh [source_root] [build_dir_with_compile_commands]
#
# Examples:
#   ./c-quality.sh .
#   ./c-quality.sh . build

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

for tool in clang-format clangd; do
  command -v "$tool" >/dev/null 2>&1 || fail "Missing tool: $tool"
done
HAS_CPPCHECK=0
if command -v cppcheck >/dev/null 2>&1; then
  HAS_CPPCHECK=1
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

  return 1
}

note "Tool versions:"
note "  clang-format: $(clang-format --version)"
note "  clangd:       $(clangd --version | head -n 1)"
if ((HAS_CPPCHECK)); then
  note "  cppcheck:     $(cppcheck --version)"
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
  note "No $CDB found (expected in repo root or build dir); skipping clangd."
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

if ((HAS_CPPCHECK)); then
  note "All required quality checks passed (cppcheck hard checks included)."
else
  note "All required quality checks passed (optional cppcheck skipped)."
fi
