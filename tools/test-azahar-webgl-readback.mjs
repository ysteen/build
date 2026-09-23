#!/usr/bin/env node
// Build a browser fixture using the actual GL state/resource/readback implementation.
// Logging and profiling are stubbed; every GPU operation uses real WebGL via Emscripten.
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

const root = resolve(fileURLToPath(new URL('..', import.meta.url)));
const azahar = process.env.AZAHAR_SOURCE || join(root, 'compile/azahar');
const output = mkdtempSync(join(tmpdir(), 'azahar-webgl-readback-'));
const shims = join(output, 'shims');
for (const directory of ['common/logging', 'video_core/host_shaders']) {
  mkdirSync(join(shims, directory), { recursive: true });
}
writeFileSync(join(shims, 'common/logging/log.h'),
  '#pragma once\n#define LOG_DEBUG(...) ((void)0)\n#define LOG_ERROR(...) ((void)0)\n');
writeFileSync(join(shims, 'common/microprofile.h'),
  '#pragma once\n#define MICROPROFILE_DEFINE(...)\n#define MICROPROFILE_SCOPE(...)\n');
writeFileSync(join(shims, 'common/assert.h'),
  '#pragma once\n#include <cassert>\n#define UNREACHABLE() assert(false)\n');
for (const name of ['webgl_readback.vert', 'webgl_readback_depth.frag', 'webgl_readback_stencil.frag']) {
  const source = readFileSync(join(azahar, 'src/video_core/host_shaders', name), 'utf8');
  const id = name.replace('.', '_');
  writeFileSync(join(shims, 'video_core/host_shaders', id + '.h'),
    `#pragma once\n#include <string_view>\nnamespace HostShaders { constexpr std::string_view ${id.toUpperCase()} = R"SHADER(${source})SHADER"; }\n`);
}
const manager = readFileSync(join(azahar, 'src/video_core/renderer_opengl/gl_shader_manager.cpp'), 'utf8');
const start = manager.indexOf('static std::set<GLenum> GetSupportedFormats()');
const end = manager.indexOf('static std::tuple', start);
assert.ok(start >= 0 && end > start, 'Missing program binary format query');
writeFileSync(join(shims, 'supported_formats.h'), '#include <set>\n' + manager.slice(start, end));

const env = { ...process.env, EM_CONFIG: process.env.EM_CONFIG || join(root, 'emsdk/.emscripten') };
const compiler = process.env.EMXX || join(root, 'emsdk/upstream/emscripten/em++');
const args = ['-std=c++20', '-O1', '-fexceptions', '-sDISABLE_EXCEPTION_CATCHING=0',
  '-sMIN_WEBGL_VERSION=2', '-sMAX_WEBGL_VERSION=2', '-sGL_ENABLE_GET_PROC_ADDRESS=1',
  '-sMODULARIZE=1', '-sEXPORT_NAME=ReadbackTest', '-sSINGLE_FILE=1', '-sENVIRONMENT=web',
  '-I' + shims, '-I' + join(azahar, 'src'), '-I' + join(azahar, 'externals/glad/include'),
  join(root, 'tools/tests/azahar-native-readback.cpp'), join(azahar, 'externals/glad/src/glad.c'),
  ...['gl_state.cpp', 'gl_shader_util.cpp', 'gl_resource_manager.cpp', 'gl_webgl_readback.cpp']
    .map(name => join(azahar, 'src/video_core/renderer_opengl', name)),
  '-o', join(output, 'test.js')];
const build = spawnSync(compiler, args, { env, stdio: 'inherit' });
if (build.error) throw build.error;
assert.equal(build.status, 0, 'Readback fixture compilation failed');
const moduleSource = readFileSync(join(output, 'test.js'), 'utf8');
writeFileSync(join(output, 'run.js'), `(async () => {
  ${moduleSource}
  const canvas = document.createElement('canvas');
  const logs = [];
  try {
    await ReadbackTest({canvas, print: text => logs.push(text), printErr: text => logs.push(text)});
    if (logs.some(text => text.startsWith('FAIL:')) || logs.filter(text => text.startsWith('PASS:')).length !== 3) {
      throw new Error(logs.join('\\n'));
    }
    return {passed: true, logs, isolatedContext: true};
  } finally {
    canvas.getContext('webgl2')?.getExtension('WEBGL_lose_context')?.loseContext();
  }
})()`);
console.log(`Evaluate ${join(output, 'run.js')} in a WebGL2 browser. This creates a separate canvas and never touches the emulator.`);
