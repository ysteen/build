#!/usr/bin/env bash
set -euo pipefail

BUILD_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AZAHAR_SOURCE="${AZAHAR_SOURCE:-$BUILD_ROOT/compile/azahar}"
EMXX="${EMXX:-$BUILD_ROOT/emsdk/upstream/emscripten/em++}"
TEST_OUTPUT="$(mktemp -d /tmp/azahar-shader-jumps-XXXXXX)"
DECOMPILER_SOURCE="${DECOMPILER_SOURCE:-$AZAHAR_SOURCE/src/video_core/shader/generator/glsl_shader_decompiler.cpp}"
STRESS_BLOCKS="${STRESS_BLOCKS:-48}"
INCLUDES=(-I"$AZAHAR_SOURCE/src" -I"$AZAHAR_SOURCE/externals/boost"
    -I"$AZAHAR_SOURCE/externals/nihstro/include" -I"$AZAHAR_SOURCE/externals/fmt/include"
    -I"$AZAHAR_SOURCE/externals/microprofile" -I"$AZAHAR_SOURCE/externals/xxHash"
    -I"$AZAHAR_SOURCE/externals/cityhash/src")
SOURCES=("$BUILD_ROOT/tools/tests/azahar-shader-jumps.cpp"
    "$DECOMPILER_SOURCE"
    "$AZAHAR_SOURCE/src/video_core/pica/shader_setup.cpp"
    "$AZAHAR_SOURCE/src/video_core/pica/shader_unit.cpp")
DEFINES=(-DMICROPROFILE_ENABLED=0 -DXXH_INLINE_ALL -DFMT_HEADER_ONLY)

"${CXX:-g++}" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    "${DEFINES[@]}" "${INCLUDES[@]}" "${SOURCES[@]}" -o "$TEST_OUTPUT/native"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$TEST_OUTPUT/native" "$STRESS_BLOCKS" > "$TEST_OUTPUT/native.json"
"$EMXX" -std=c++20 -O2 -sENVIRONMENT=node -sSTACK_SIZE=1048576 -sSTACK_OVERFLOW_CHECK=2 \
    -include emscripten.h "${DEFINES[@]}" "${INCLUDES[@]}" "${SOURCES[@]}" -o "$TEST_OUTPUT/wasm.js"
node "$TEST_OUTPUT/wasm.js" "$STRESS_BLOCKS" > "$TEST_OUTPUT/wasm.json"
node -e 'const fs=require("fs"); const rows=process.argv.slice(1).map(p=>JSON.parse(fs.readFileSync(p))); if(JSON.stringify(rows[0].map(x=>x.cases))!==JSON.stringify(rows[1].map(x=>x.cases))) throw Error("Native/Wasm reference mismatch"); console.log("PASS: native/Wasm expected outputs match");' "$TEST_OUTPUT/native.json" "$TEST_OUTPUT/wasm.json"
echo "Shader jump fixtures: $TEST_OUTPUT"
