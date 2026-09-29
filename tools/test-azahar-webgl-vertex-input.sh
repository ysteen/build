#!/usr/bin/env bash
set -euo pipefail
BUILD_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AZAHAR_SOURCE="${AZAHAR_SOURCE:-$BUILD_ROOT/compile/azahar}"
EMXX="${EMXX:-$BUILD_ROOT/emsdk/upstream/emscripten/em++}"
TEST_OUTPUT="${TEST_OUTPUT:-$(mktemp -d /tmp/azahar-vertex-input-XXXXXX)}"
mkdir -p "$TEST_OUTPUT"
INCLUDES=(-I"$AZAHAR_SOURCE/src" -I"$AZAHAR_SOURCE/externals/boost"
    -I"$AZAHAR_SOURCE/externals/fmt/include")
SOURCE="$BUILD_ROOT/tools/tests/azahar-webgl-vertex-input.cpp"
"${CXX:-g++}" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    "${INCLUDES[@]}" "$SOURCE" -o "$TEST_OUTPUT/native"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$TEST_OUTPUT/native" > "$TEST_OUTPUT/native.json"
"$EMXX" -std=c++20 -O2 -sENVIRONMENT=node "${INCLUDES[@]}" "$SOURCE" -o "$TEST_OUTPUT/fixture.js"
node "$TEST_OUTPUT/fixture.js" > "$TEST_OUTPUT/wasm.json"
cmp "$TEST_OUTPUT/native.json" "$TEST_OUTPUT/wasm.json"
node - "$TEST_OUTPUT" "$BUILD_ROOT/tools/tests/azahar-webgl-vertex-input.js" <<'JS'
const fs = require('fs'), [directory, harness] = process.argv.slice(2);
const fixtures = JSON.parse(fs.readFileSync(directory+'/wasm.json', 'utf8'));
fs.writeFileSync(directory+'/verify.js', '('+fs.readFileSync(harness,'utf8')+')('+JSON.stringify(fixtures)+')');
console.log(`PASS: ${fixtures.length} native/Wasm conversions, padding, unaligned reads, float bits and bounds`);
JS
echo "GPU fixture: $TEST_OUTPUT/verify.js"
