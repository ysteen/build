#!/usr/bin/env bash
set -euo pipefail

BUILD_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AZAHAR_SOURCE="${AZAHAR_SOURCE:-$BUILD_ROOT/compile/azahar}"
EMXX="${EMXX:-$BUILD_ROOT/emsdk/upstream/emscripten/em++}"
TEST_OUTPUT="$(mktemp -d /tmp/azahar-hotpaths-XXXXXX)"
TEST_SOURCE="$BUILD_ROOT/tools/tests/azahar-hotpaths.cpp"
INCLUDES=(-I"$AZAHAR_SOURCE/src" -I"$AZAHAR_SOURCE/externals/boost")

"${CXX:-g++}" -std=c++20 -O2 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    "${INCLUDES[@]}" "$TEST_SOURCE" -o "$TEST_OUTPUT/native"
# LeakSanitizer cannot run under the debugger used by the workspace terminal.
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$TEST_OUTPUT/native" --check-only

"$EMXX" -std=c++20 -O3 -flto=thin -pthread -msimd128 -sENVIRONMENT=node \
    "${INCLUDES[@]}" "$TEST_SOURCE" -o "$TEST_OUTPUT/wasm.js"
node "$TEST_OUTPUT/wasm.js" | tee "$TEST_OUTPUT/results.txt"

STREAM_INCLUDES=("${INCLUDES[@]}" -I"$AZAHAR_SOURCE/externals/glad/include"
    -I"$AZAHAR_SOURCE/externals/xxHash" -I"$AZAHAR_SOURCE/externals/cityhash/src"
    -I"$AZAHAR_SOURCE/externals/fmt/include" -I"$AZAHAR_SOURCE/externals/microprofile")
STREAM_SOURCES=("$BUILD_ROOT/tools/tests/azahar-stream-buffer.cpp"
    "$AZAHAR_SOURCE/src/video_core/renderer_opengl/gl_stream_buffer.cpp")
"${CXX:-g++}" -std=c++20 -O2 -g -D__EMSCRIPTEN__ -DMICROPROFILE_ENABLED=0 \
    -fsanitize=address,undefined -fno-omit-frame-pointer -DAZAHAR_TEST_GL -DXXH_INLINE_ALL "${STREAM_INCLUDES[@]}" \
    "${STREAM_SOURCES[@]}" -o "$TEST_OUTPUT/stream-native"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$TEST_OUTPUT/stream-native"
"$EMXX" -std=c++20 -O3 -flto=thin -pthread -msimd128 -sENVIRONMENT=node \
    -DMICROPROFILE_ENABLED=0 -DAZAHAR_TEST_GL -DXXH_INLINE_ALL "${STREAM_INCLUDES[@]}" "${STREAM_SOURCES[@]}" \
    -o "$TEST_OUTPUT/stream-wasm.js"
node "$TEST_OUTPUT/stream-wasm.js"
echo "Validation and benchmark artifacts: $TEST_OUTPUT"
