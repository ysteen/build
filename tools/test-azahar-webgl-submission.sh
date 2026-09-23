#!/usr/bin/env bash
set -euo pipefail
BUILD_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AZAHAR_SOURCE="${AZAHAR_SOURCE:-$BUILD_ROOT/compile/azahar}"
EMXX="${EMXX:-$BUILD_ROOT/emsdk/upstream/emscripten/em++}"
TEST_OUTPUT="${TEST_OUTPUT:-$(mktemp -d /tmp/azahar-webgl-submission-XXXXXX)}"
mkdir -p "$TEST_OUTPUT"
INCLUDES=(-I"$AZAHAR_SOURCE/src" -I"$AZAHAR_SOURCE/externals/boost"
    -I"$AZAHAR_SOURCE/externals/glad/include" -I"$AZAHAR_SOURCE/externals/fmt/include"
    -I"$AZAHAR_SOURCE/externals/microprofile" -I"$AZAHAR_SOURCE/externals/xxHash"
    -I"$AZAHAR_SOURCE/externals/cityhash/src")
"$EMXX" -std=c++20 -O2 -fexceptions -msimd128 -sASYNCIFY=1 \
    -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sENVIRONMENT=web \
    -sMODULARIZE=1 -sEXPORT_NAME=createSubmissionTest -sSINGLE_FILE=1 \
    -DMICROPROFILE_ENABLED=0 -DXXH_INLINE_ALL "${INCLUDES[@]}" \
    "$BUILD_ROOT/tools/tests/azahar-webgl-submission.cpp" \
    "$AZAHAR_SOURCE/src/video_core/renderer_opengl/gl_stream_buffer.cpp" \
    "$AZAHAR_SOURCE/externals/glad/src/glad.c" -o "$TEST_OUTPUT/fixture.js"
node - "$TEST_OUTPUT" <<'JS'
const fs = require('fs'), path = require('path');
const dir = process.argv[2];
const source = fs.readFileSync(path.join(dir, 'fixture.js'), 'utf8');
fs.writeFileSync(path.join(dir, 'verify-submission.js'), `(async()=>{
const canvas=document.createElement('canvas');canvas.id='canvas';canvas.width=16;canvas.height=16;document.body.append(canvas);
const logs=[];${source}\nawait createSubmissionTest({canvas,print:s=>logs.push(s),printErr:s=>logs.push(s)});
return {...globalThis.__SUBMISSION_TEST_RESULT__,logs};})()`);
JS
echo "Evaluate $TEST_OUTPUT/verify-submission.js in an isolated WebGL2 tab."
