#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
#if defined(__wasm__)
#include <emscripten.h>
#endif
#include "common/hash.h"
#include "common/logging/log.h"
#include "video_core/renderer_opengl/gl_driver.h"
#include "video_core/renderer_opengl/gl_stream_buffer.h"

// Compile the real WebGL stream implementation against a checked, CPU-only GL backend.
static void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::abort();
    }
}

static std::vector<u8> storage;
static GLenum bound_target;
static unsigned allocations;
static unsigned uploads;

static void APIENTRY BindBuffer(GLenum target, GLuint handle) {
    Check(handle == 1, "buffer binding");
    bound_target = target;
}

static void APIENTRY BufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    Check(target == bound_target && data == nullptr && usage == GL_STREAM_DRAW,
          "stream allocation target and usage");
    storage.assign(size, 0xcd);
    ++allocations;
}

static void APIENTRY BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size,
                                  const void* data) {
    Check(target == bound_target && offset >= 0 && size > 0 &&
              static_cast<std::size_t>(offset + size) <= storage.size(),
          "upload bounds and target");
    std::copy_n(static_cast<const u8*>(data), size, storage.begin() + offset);
    ++uploads;
}

static GLboolean APIENTRY UnmapBuffer(GLenum) {
    Check(false, "WebGL must not use native buffer mapping");
    return GL_FALSE;
}

PFNGLBINDBUFFERPROC glad_glBindBuffer = BindBuffer;
PFNGLBUFFERDATAPROC glad_glBufferData = BufferData;
PFNGLBUFFERSUBDATAPROC glad_glBufferSubData = BufferSubData;
PFNGLUNMAPBUFFERPROC glad_glUnmapBuffer = UnmapBuffer;

namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char*, unsigned int, const char*, fmt::string_view,
                       const fmt::format_args&) {
    Check(false, "stream assertion");
}
void Stop() {}
} // namespace Common::Log

namespace OpenGL {
Driver::Driver() = default;
Driver::~Driver() = default;
void OGLBuffer::Create() { handle = 1; }
void OGLBuffer::Release() { handle = 0; }
} // namespace OpenGL

static void CheckStream(GLenum target, GLenum expected_target) {
    allocations = uploads = 0;
    OpenGL::Driver driver;
    OpenGL::OGLStreamBuffer buffer{driver, target, 64};
    Check(bound_target == expected_target && allocations == 1, "initial allocation and fixup");
    auto [bytes, offset, invalidate] = buffer.Map(32);
    Check(offset == 0 && !invalidate, "initial map");
    std::fill_n(bytes, 8, 0x11);
    buffer.Unmap(8);
    Check(storage[7] == 0x11 && storage[8] == 0xcd, "upload only used bytes");

    std::tie(bytes, offset, invalidate) = buffer.Map(16, 16);
    Check(offset == 16 && !invalidate, "aligned append");
    std::fill_n(bytes, 12, 0x22);
    buffer.Unmap(12);
    Check(storage[16] == 0x22 && storage[27] == 0x22 && storage[28] == 0xcd,
          "aligned data range");

    std::tie(bytes, offset, invalidate) = buffer.Map(40);
    Check(offset == 0 && invalidate && allocations == 1, "wrap marks old ranges invalid");
    std::fill_n(bytes, 40, 0x33);
    buffer.Unmap(40);
    Check(allocations == 2 && storage[0] == 0x33 && storage[39] == 0x33,
          "wrap orphans before upload");

    std::tie(bytes, offset, invalidate) = buffer.Map(96, 16);
    Check(offset == 0 && invalidate && buffer.GetSize() == 96 && allocations == 3,
          "growth invalidates uniform bindings and allocates once");
    std::fill_n(bytes, 16, 0x44);
    buffer.Unmap(16);
    Check(allocations == 3 && storage[15] == 0x44, "growth does not orphan twice");

    std::tie(bytes, offset, invalidate) = buffer.Map(0);
    buffer.Unmap(0);
    Check(offset == 16 && !invalidate && uploads == 4, "empty upload");

    std::tie(bytes, offset, invalidate) = buffer.Map(80, 16);
    Check(offset == 16 && !invalidate, "exact-end allocation");
    std::fill_n(bytes, 80, 0x55);
    buffer.Unmap(80);
    std::tie(bytes, offset, invalidate) = buffer.Map(1, 16);
    Check(offset == 0 && invalidate, "wrap after exact-end allocation");
    *bytes = 0x66;
    buffer.Unmap(1);
    Check(allocations == 4 && uploads == 6 && storage[0] == 0x66, "final upload");
}

static void CheckReuse(GLenum target) {
    allocations = uploads = 0;
    OpenGL::Driver driver;
    OpenGL::OGLStreamBuffer buffer{driver, target, 64};
    const std::vector<u8> first(16, 0x31), second(16, 0x72), third(16, 0x19);
    const auto a = buffer.UploadCached(first, 16);
    Check(a == 0 && uploads == 1, "first cached upload");
    Check(buffer.UploadCached(first, 16) == a && uploads == 1, "identical bytes reuse GPU range");
    const auto b = buffer.UploadCached(second, 16);
    Check(b == 16 && uploads == 2, "different bytes upload");
    Check(buffer.UploadCached(first, 16) == a && uploads == 2, "reuse after intervening upload");
    const auto c = buffer.UploadCached(third, 32);
    Check(c == 32 && uploads == 3, "cached alignment");
    Check(buffer.UploadCached(third, 64) == 0 && allocations == 2 && uploads == 4,
          "incompatible alignment wraps and reuploads");
    Check(buffer.UploadCached(first, 16) == 16 && uploads == 5,
          "orphan invalidates old cache entries");
    auto [bytes, offset, invalid] = buffer.Map(64);
    std::fill_n(bytes, 64, 0x44);
    buffer.Unmap(64);
    Check(invalid && allocations == 3, "uncached write wraps cached storage");
    buffer.UploadCached(first, 16);
    Check(uploads == 7 && allocations == 4 && storage[0] == 0x31,
          "mixed uncached writes cannot create stale cache hits");
    const std::vector<u8> large(96, 0x55);
    buffer.UploadCached(large, 16);
    Check(buffer.GetSize() == 96 && uploads == 8 && allocations == 5, "cached growth");
    buffer.UploadCached(large, 16);
    Check(uploads == 8 && allocations == 5, "reuse full buffer before wrapping");
    buffer.UploadCached(first, 16);
    Check(uploads == 9 && storage[0] == 0x31, "growth invalidates prior entries");

    const auto bucket = Common::ComputeHash64(first.data(), first.size()) % 256;
    std::vector<u8> collision(16, 0);
    for (unsigned i = 1;; ++i) {
        std::memcpy(collision.data(), &i, sizeof(i));
        if (Common::ComputeHash64(collision.data(), collision.size()) % 256 == bucket) break;
        Check(i < 100000, "find bucket collision");
    }
    buffer.UploadCached(collision, 16);
    const auto again = buffer.UploadCached(first, 16);
    Check(uploads == 11 && std::equal(first.begin(), first.end(), storage.begin() + again),
          "bucket collisions preserve bytes");
}

int main() {
    CheckStream(GL_ARRAY_BUFFER, GL_ARRAY_BUFFER);
    CheckStream(GL_UNIFORM_BUFFER, GL_UNIFORM_BUFFER);
    CheckStream(GL_ELEMENT_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER);
    CheckStream(GL_TEXTURE_BUFFER, GL_ARRAY_BUFFER);
    CheckReuse(GL_ARRAY_BUFFER);
    CheckReuse(GL_ELEMENT_ARRAY_BUFFER);
    CheckReuse(GL_UNIFORM_BUFFER);
    std::puts("PASS: WebGL stream alignment, partial upload, wrap, growth and invalidation");
    std::puts("PASS: upload reuse, collision, alignment, eviction and mixed cached/uncached writes");
}
