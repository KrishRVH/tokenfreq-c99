#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 4)}"

run_preset() {
  local preset="$1"
  echo "----------------------------------------------------"
  echo "Running preset: $preset"
  echo "----------------------------------------------------"

  cmake --preset "$preset"
  cmake --build --preset "$preset" --parallel "$JOBS"

  # Skip running Windows binaries by default
  if [[ "$preset" != "mingw" ]]; then
    ctest --preset "$preset"
  fi
}

presets=(clang clang-stronghash gcc)

if command -v ccomp >/dev/null 2>&1; then
  presets+=(compcert)
else
  echo "[INFO] ccomp not found; skipping CompCert preset."
fi

if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
  presets+=(mingw)
else
  echo "[INFO] x86_64-w64-mingw32-gcc not found; skipping MinGW preset."
fi

for p in "${presets[@]}"; do
  run_preset "$p"
done

# Quality checks: run on repo sources, use build/clang if present
echo "----------------------------------------------------"
echo "Running quality checks"
echo "----------------------------------------------------"
if [[ -f "$ROOT/build/clang/compile_commands.json" ]]; then
  "$ROOT/c-quality.sh" "$ROOT" "$ROOT/build/clang"
else
  "$ROOT/c-quality.sh" "$ROOT"
fi

echo "✅ GAMUT COMPLETE"
