#!/usr/bin/env bash
# Builds aios-asql-crash-log-check (and only it) and runs it; the exit status is the check's.
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD="${TMPDIR:-/tmp}/aios-asql-crash-log-check-build"
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DAIOS_ENABLE_SDL2=OFF > "$BUILD.cmake.log" 2>&1 || { tail -20 "$BUILD.cmake.log" >&2; exit 1; }
cmake --build "$BUILD" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" --target aios-asql-crash-log-check > "$BUILD.make.log" 2>&1 || { grep -E "error|undefined" "$BUILD.make.log" | head -30 >&2; exit 1; }
exec "$BUILD/aios-asql-crash-log-check"
