#!/usr/bin/env bash
set -euo pipefail
BUILD_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AZAHAR_SOURCE="${AZAHAR_SOURCE:-$BUILD_ROOT/compile/azahar}"
EMXX="${EMXX:-$BUILD_ROOT/emsdk/upstream/emscripten/em++}"
TEST_OUTPUT="${TEST_OUTPUT:-$(mktemp -d /tmp/azahar-webgl-cache-XXXXXX)}"
mkdir -p "$TEST_OUTPUT"
TEST_OUTPUT="$(cd "$TEST_OUTPUT" && pwd)"
INCLUDES=(-I"$AZAHAR_SOURCE/src" -I"$AZAHAR_SOURCE/externals/boost"
    -I"$AZAHAR_SOURCE/externals/nihstro/include" -I"$AZAHAR_SOURCE/externals/fmt/include"
    -I"$AZAHAR_SOURCE/externals/microprofile" -I"$AZAHAR_SOURCE/externals/xxHash"
    -I"$AZAHAR_SOURCE/externals/cityhash/src")
DEFINES=(-DMICROPROFILE_ENABLED=0 -DXXH_INLINE_ALL -DFMT_HEADER_ONLY)
"${CXX:-g++}" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    "${DEFINES[@]}" "${INCLUDES[@]}" "$BUILD_ROOT/tools/tests/azahar-webgl-cache.cpp" \
    -o "$TEST_OUTPUT/cache-native"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$TEST_OUTPUT/cache-native"
"$EMXX" -std=c++20 -O2 -sENVIRONMENT=node -sALLOW_MEMORY_GROWTH=1 "${DEFINES[@]}" "${INCLUDES[@]}" \
    "$BUILD_ROOT/tools/tests/azahar-webgl-cache.cpp" -o "$TEST_OUTPUT/cache-wasm.js"
node "$TEST_OUTPUT/cache-wasm.js"
"$EMXX" -std=c++20 -O2 -sENVIRONMENT=node -sSTACK_SIZE=1048576 \
    "${DEFINES[@]}" "${INCLUDES[@]}" "$BUILD_ROOT/tools/tests/azahar-fragment-variants.cpp" \
    "${FS_GENERATOR_SOURCE:-$AZAHAR_SOURCE/src/video_core/shader/generator/glsl_fs_shader_gen.cpp}" \
    "${FS_CONFIG_SOURCE:-$AZAHAR_SOURCE/src/video_core/shader/generator/pica_fs_config.cpp}" \
    -o "$TEST_OUTPUT/fragments.js"
node "$TEST_OUTPUT/fragments.js" > "$TEST_OUTPUT/fragments.json"

# Undo only this change in temporary copies; leave the working core untouched.
GENERATOR=src/video_core/shader/generator/glsl_fs_shader_gen.cpp
CONFIG=src/video_core/shader/generator/pica_fs_config.cpp
mkdir -p "$TEST_OUTPUT/before/src/video_core/shader/generator"
cp "$AZAHAR_SOURCE/$GENERATOR" "$TEST_OUTPUT/before/$GENERATOR"
cp "$AZAHAR_SOURCE/$CONFIG" "$TEST_OUTPUT/before/$CONFIG"
git -C "$TEST_OUTPUT/before" apply --reverse --include="$GENERATOR" --include="$CONFIG" \
    "$BUILD_ROOT/patches/azahar-webgl-async.patch"
git -C "$TEST_OUTPUT/before" apply --reverse --include="$GENERATOR" --include="$CONFIG" \
    "$BUILD_ROOT/patches/azahar-webgl-cache.patch"
"$EMXX" -std=c++20 -O2 -sENVIRONMENT=node -sSTACK_SIZE=1048576 \
    "${DEFINES[@]}" "${INCLUDES[@]}" "$BUILD_ROOT/tools/tests/azahar-fragment-variants.cpp" \
    "$TEST_OUTPUT/before/$GENERATOR" "$TEST_OUTPUT/before/$CONFIG" \
    -o "$TEST_OUTPUT/fragments-before.js"
node "$TEST_OUTPUT/fragments-before.js" > "$TEST_OUTPUT/fragments-before.json"
for variant in before after; do
    SOURCE_ROOT="$AZAHAR_SOURCE"
    if [[ "$variant" == before ]]; then SOURCE_ROOT="$TEST_OUTPUT/before"; fi
    "${CXX:-g++}" -std=c++20 -O1 "${DEFINES[@]}" "${INCLUDES[@]}" \
        "$BUILD_ROOT/tools/tests/azahar-fragment-variants.cpp" \
        "$SOURCE_ROOT/$GENERATOR" "$SOURCE_ROOT/$CONFIG" -o "$TEST_OUTPUT/native-$variant"
    "$TEST_OUTPUT/native-$variant" > "$TEST_OUTPUT/native-$variant.json"
done
cmp "$TEST_OUTPUT/native-before.json" "$TEST_OUTPUT/native-after.json"
echo "PASS: native fragment shader output is unchanged"

node - "$TEST_OUTPUT" "$BUILD_ROOT/tools/tests/azahar-fragment-variants.js" <<'JS'
const fs = require('fs'), path = require('path');
const [dir, harness] = process.argv.slice(2);
const rows = ['fragments-before.json', 'fragments.json'].map(name =>
    JSON.stringify(JSON.parse(fs.readFileSync(path.join(dir, name), 'utf8'))));
fs.writeFileSync(path.join(dir, 'verify-fragments.js'),
    '(' + fs.readFileSync(harness, 'utf8') + ')(' + rows.join(',') + ')');
JS
echo "Evaluate $TEST_OUTPUT/verify-fragments.js in an idle WebGL2 test page (await the result)."
