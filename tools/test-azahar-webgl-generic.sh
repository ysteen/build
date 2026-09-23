#!/usr/bin/env bash
set -euo pipefail
BUILD_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AZAHAR_SOURCE="${AZAHAR_SOURCE:-$BUILD_ROOT/compile/azahar}"
EMXX="${EMXX:-$BUILD_ROOT/emsdk/upstream/emscripten/em++}"
TEST_OUTPUT="${TEST_OUTPUT:-$(mktemp -d /tmp/azahar-webgl-generic-XXXXXX)}"
mkdir -p "$TEST_OUTPUT"
TEST_OUTPUT="$(cd "$TEST_OUTPUT" && pwd)"
INCLUDES=(-I"$AZAHAR_SOURCE/src" -I"$AZAHAR_SOURCE/externals/boost"
    -I"$AZAHAR_SOURCE/externals/nihstro/include" -I"$AZAHAR_SOURCE/externals/fmt/include"
    -I"$AZAHAR_SOURCE/externals/microprofile" -I"$AZAHAR_SOURCE/externals/xxHash"
    -I"$AZAHAR_SOURCE/externals/cityhash/src")
"${CXX:-g++}" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -D__EMSCRIPTEN__ -DMICROPROFILE_ENABLED=0 -DFMT_HEADER_ONLY "${INCLUDES[@]}" \
    -I"$AZAHAR_SOURCE/externals/glad/include" \
    "$BUILD_ROOT/tools/tests/azahar-webgl-async-util.cpp" \
    "$AZAHAR_SOURCE/src/video_core/renderer_opengl/gl_shader_util.cpp" \
    "$AZAHAR_SOURCE/externals/glad/src/glad.c" -ldl -o "$TEST_OUTPUT/async-util"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$TEST_OUTPUT/async-util"
"$EMXX" -std=c++20 -O2 -sENVIRONMENT=node -sSTACK_SIZE=1048576 \
    -DMICROPROFILE_ENABLED=0 -DXXH_INLINE_ALL -DFMT_HEADER_ONLY "${INCLUDES[@]}" \
    "$BUILD_ROOT/tools/tests/azahar-webgl-generic.cpp" \
    "$AZAHAR_SOURCE/src/video_core/shader/generator/glsl_fs_shader_gen.cpp" \
    "$AZAHAR_SOURCE/src/video_core/shader/generator/pica_fs_config.cpp" \
    "$AZAHAR_SOURCE/src/video_core/shader/generator/glsl_webgl_generic.cpp" \
    -o "$TEST_OUTPUT/fixtures.js"
node "$TEST_OUTPUT/fixtures.js" > "$TEST_OUTPUT/fixtures.json"
node - "$TEST_OUTPUT" "$BUILD_ROOT/tools/tests/azahar-webgl-generic.js" <<'JS'
const fs = require('fs'), path = require('path');
const [dir, harness] = process.argv.slice(2);
const input = JSON.parse(fs.readFileSync(path.join(dir, 'fixtures.json'), 'utf8'));
fs.writeFileSync(path.join(dir, 'verify-generic.js'),
    '(' + fs.readFileSync(harness, 'utf8') + ')(' + JSON.stringify(input) + ')');
console.log(`PASS: ${input.fixtures.length} generic configurations; ${input.unsupportedRejected} unsupported configurations rejected`);
JS
echo "Evaluate $TEST_OUTPUT/verify-generic.js in a separate WebGL2 test page (await the result)."
