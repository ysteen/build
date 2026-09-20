#!/usr/bin/env bash
set -euo pipefail

BUILD_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AZAHAR_SOURCE="${AZAHAR_SOURCE:-$BUILD_ROOT/compile/azahar}"
EMXX="${EMXX:-$BUILD_ROOT/emsdk/upstream/emscripten/em++}"
TEST_OUTPUT="$(mktemp -d /tmp/azahar-shader-decoder-XXXXXX)"
INCLUDES=(-I"$AZAHAR_SOURCE/src" -I"$AZAHAR_SOURCE/externals/boost"
    -I"$AZAHAR_SOURCE/externals/nihstro/include" -I"$AZAHAR_SOURCE/externals/fmt/include"
    -I"$AZAHAR_SOURCE/externals/microprofile" -I"$AZAHAR_SOURCE/externals/xxHash"
    -I"$AZAHAR_SOURCE/externals/cityhash/src")
if [[ -n "${AZAHAR_SHADER_INCLUDE_OVERRIDE:-}" ]]; then
    # Compare a saved interpreter/header snapshot with the same tests and compiler flags.
    INCLUDES=(-I"$AZAHAR_SHADER_INCLUDE_OVERRIDE" "${INCLUDES[@]}")
fi
SOURCES=("$BUILD_ROOT/tools/tests/azahar-shader-decoder.cpp"
    "$AZAHAR_SOURCE/src/video_core/pica/shader_setup.cpp"
    "$AZAHAR_SOURCE/src/video_core/pica/shader_unit.cpp")
DEFINES=(-DMICROPROFILE_ENABLED=0 -DXXH_INLINE_ALL)

"${CXX:-g++}" -std=c++20 -O2 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    "${DEFINES[@]}" "${INCLUDES[@]}" "${SOURCES[@]}" -o "$TEST_OUTPUT/native"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$TEST_OUTPUT/native" --check-only
"$EMXX" -std=c++20 -O3 -flto=thin -pthread -msimd128 -sENVIRONMENT=node \
    -sSTACK_SIZE=1048576 -sSTACK_OVERFLOW_CHECK=2 \
    -include emscripten.h \
    "${DEFINES[@]}" "${INCLUDES[@]}" "${SOURCES[@]}" -o "$TEST_OUTPUT/wasm.js"
node "$TEST_OUTPUT/wasm.js" | tee "$TEST_OUTPUT/results.txt"
echo "Shader decoder validation artifacts: $TEST_OUTPUT"
