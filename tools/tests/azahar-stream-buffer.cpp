#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
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

int main() {
    CheckStream(GL_ARRAY_BUFFER, GL_ARRAY_BUFFER);
    CheckStream(GL_UNIFORM_BUFFER, GL_UNIFORM_BUFFER);
    CheckStream(GL_ELEMENT_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER);
    CheckStream(GL_TEXTURE_BUFFER, GL_ARRAY_BUFFER);
    std::puts("PASS: WebGL stream alignment, partial upload, wrap, growth and invalidation");
}
