#!/usr/bin/env node
// Exercise the actual Surface download functions with deterministic GL mocks.
// The separate browser fixture validates the real depth/stencil shader output.
import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

const root = resolve(fileURLToPath(new URL('..', import.meta.url)));
const azahar = process.env.AZAHAR_SOURCE || join(root, 'compile/azahar');
const output = mkdtempSync(join(tmpdir(), 'azahar-readback-state-'));
const source = readFileSync(join(azahar,
  'src/video_core/renderer_opengl/gl_texture_runtime.cpp'), 'utf8');
const start = source.indexOf('void Surface::Download(');
const end = source.indexOf('void Surface::Attach(', start);
assert.ok(start >= 0 && end > start, 'Missing Surface readback implementation');
const download = source.slice(start, end);
assert.doesNotMatch(download, /glPixelStorei\(GL_UNPACK_/);

writeFileSync(join(output, 'readback.cpp'), `
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include "common/scope_exit.h"
using u8 = uint8_t;
using u32 = uint32_t;
using GLint = int;
using GLenum = int;
using GLsizei = int;
#define ASSERT assert
constexpr GLenum GL_PACK_ROW_LENGTH = 1, GL_UNPACK_ROW_LENGTH = 2;
constexpr GLenum GL_READ_FRAMEBUFFER = 3, GL_TEXTURE_2D = 4, TEMP_UNIT = 15;
static GLint pack = 11, unpack = 17;
static int reads = 0, blits = 0;
static bool fail_read = false;
static void glGetIntegerv(GLenum name, GLint* value) {
    assert(name == GL_PACK_ROW_LENGTH); *value = pack;
}
static void glPixelStorei(GLenum name, GLint value) {
    assert(name == GL_PACK_ROW_LENGTH); pack = value;
}
static void glActiveTexture(GLenum) {}
static void read() {
    assert(pack == 4); assert(unpack == 17); ++reads;
    if (fail_read) throw std::runtime_error("test readback failure");
}
template<class... Args> void glReadPixels(Args...) { read(); }
template<class... Args> void glGetTextureSubImage(Args...) { read(); }
template<class... Args> void glGetTexImage(Args...) { read(); }
struct Rect {
    u32 left = 0, top = 4, right = 4, bottom = 0;
    u32 GetWidth() const { return right - left; }
    u32 GetHeight() const { return top - bottom; }
    Rect operator*(u32 n) const { return {left*n, top*n, right*n, bottom*n}; }
    bool operator==(const Rect&) const = default;
};
namespace VideoCore {
struct BufferTextureCopy { u32 texture_level = 0; Rect texture_rect; };
struct StagingData { std::span<u8> mapped; };
struct TextureBlit { u32 src_level, dst_level; Rect src_rect, dst_rect; };
}
enum class PixelFormat { RGBA8, D24S8 };
static u32 GetFormatBytesPerPixel(PixelFormat) { return 4; }
static u32 FboIndex(int) { return 0; }
struct WebGLDepthStencilReadback {
    void Download(u32, u32, u32, u32, Rect, std::span<u8>) { read(); }
};
struct Runtime {
    std::unique_ptr<WebGLDepthStencilReadback> depth_stencil_readback;
    struct { u32 handle = 5; } read_fbos[1];
    struct Tuple { GLenum format = 6, type = 7; } tuple;
    const Tuple& GetFormatTuple(PixelFormat) { return tuple; }
};
struct Driver {
    bool es = true, subimage = false;
    bool IsOpenGLES() const { return es; }
    bool HasArbGetTextureSubImage() const { return subimage; }
};
struct OpenGLState {
    struct { bool enabled = true; } scissor;
    struct { u32 read_framebuffer = 0; } draw;
    struct { u32 texture_2d = 0; } texture_units[1];
    static OpenGLState GetCurState() { return {}; }
    void Apply() {}
};
struct Surface {
    u32 stride = 4, res_scale = 1;
    PixelFormat pixel_format = PixelFormat::RGBA8;
    Runtime* runtime;
    Driver* driver;
    int type = 0;
    Rect rect{};
    u32 Handle(u32) { return 8; }
    Rect GetRect(u32 = 0) { return rect; }
    void BlitScale(const VideoCore::TextureBlit&, bool) {
        assert(pack == 4); assert(unpack == 17); ++blits;
    }
    void Attach(GLenum, u32, u32, bool) {}
    void Download(const VideoCore::BufferTextureCopy&, const VideoCore::StagingData&);
    bool DownloadWithoutFbo(const VideoCore::BufferTextureCopy&, const VideoCore::StagingData&);
};
${download}
int main() {
    std::array<u8, 64> bytes{};
    Runtime runtime;
    Driver driver;
    Surface surface{.runtime = &runtime, .driver = &driver};
    const VideoCore::BufferTextureCopy request;
    const VideoCore::StagingData staging{bytes};
    const auto check = [&] {
        const int before = reads;
        bool threw = false;
        try { surface.Download(request, staging); }
        catch (const std::runtime_error&) { threw = true; }
        assert(threw == fail_read);
        assert(reads == before + 1);
        assert(pack == 11);
        assert(unpack == 17);
    };
    check(); // GLES color FBO path.
    surface.res_scale = 2;
    check(); assert(blits == 1);
    surface.res_scale = 1;
    fail_read = true; check(); fail_read = false;
#ifdef __EMSCRIPTEN__
    surface.pixel_format = PixelFormat::D24S8;
    check(); // The helper's early return must also restore PACK state.
    fail_read = true; check(); fail_read = false;
    surface.pixel_format = PixelFormat::RGBA8;
#endif
    driver.es = false;
    driver.subimage = true;
    check(); // Desktop subimage early return.
    fail_read = true; check(); fail_read = false;
    driver.subimage = false;
    check(); // Full texture early return.
    surface.rect.right = 8;
    check(); // Partial texture fallback to FBO.
    fail_read = true; check();
}
`);
for (const target of ['native', 'web']) {
  const args = ['-std=c++20', '-Wall', '-Wextra', '-Werror', '-I' + join(azahar, 'src'),
    ...(target === 'web' ? ['-D__EMSCRIPTEN__'] : []), 'readback.cpp', '-o', target];
  for (const [command, commandArgs] of [[process.env.CXX || 'g++', args], [join(output, target), []]]) {
    const result = spawnSync(command, commandArgs, { cwd: output, stdio: 'inherit' });
    if (result.error) throw result.error;
    assert.equal(result.status, 0, `${target}: ${command} failed`);
  }
}
console.log('PASS: PACK row length restored on FBO, direct, depth, scale and failure paths; UNPACK untouched');
