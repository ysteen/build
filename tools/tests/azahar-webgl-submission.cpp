#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <emscripten.h>
#include <emscripten/html5.h>
#include "common/logging/log.h"
#include "video_core/renderer_opengl/gl_driver.h"
#include "video_core/renderer_opengl/gl_stream_buffer.h"
#include "video_core/renderer_opengl/gl_webgl_direct.h"

static void Check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::abort(); }
}

namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char*, unsigned int, const char*, fmt::string_view,
                       const fmt::format_args&) { Check(false, "stream assertion"); }
void Stop() {}
}
namespace OpenGL {
Driver::Driver() = default;
Driver::~Driver() = default;
void OGLBuffer::Create() { glGenBuffers(1, &handle); }
void OGLBuffer::Release() { glDeleteBuffers(1, &handle); handle = 0; }
}

static GLuint Shader(GLenum type, const char* source) {
    const auto shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    Check(ok, "fixture shader compilation");
    return shader;
}

template <typename T>
static GLintptr Upload(OpenGL::OGLStreamBuffer& buffer, const T& data, GLintptr alignment = 4) {
    if (OpenGL::WebGLSubmission::ReuseUploads()) {
        return buffer.UploadCached({reinterpret_cast<const u8*>(&data), sizeof(data)}, alignment);
    }
    const auto [bytes, offset, invalid] = buffer.Map(sizeof(data), alignment);
    std::memcpy(bytes, &data, sizeof(data));
    buffer.Unmap(sizeof(data));
    return offset;
}

int main() {
    EmscriptenWebGLContextAttributes attributes;
    emscripten_webgl_init_context_attributes(&attributes);
    attributes.majorVersion = 2;
    attributes.antialias = false;
    attributes.depth = false;
    attributes.stencil = false;
    const auto context = emscripten_webgl_create_context("#canvas", &attributes);
    Check(context > 0, "WebGL2 context");
    Check(emscripten_webgl_make_context_current(context) == EMSCRIPTEN_RESULT_SUCCESS,
          "current WebGL2 context");
    Check(gladLoadGLES2Loader(reinterpret_cast<GLADloadproc>(emscripten_webgl_get_proc_address)),
          "real Emscripten GLAD dispatch");
    const auto vs = Shader(GL_VERTEX_SHADER, "#version 300 es\nlayout(location=0) in vec2 p;void main(){gl_Position=vec4(p,0,1);}");
    const auto fs = Shader(GL_FRAGMENT_SHADER, "#version 300 es\nprecision highp float;layout(std140) uniform Color{vec4 c;};out vec4 outColor;void main(){outColor=c;}");
    const auto program = glCreateProgram();
    glAttachShader(program, vs); glAttachShader(program, fs); glLinkProgram(program);
    GLint ok; glGetProgramiv(program, GL_LINK_STATUS, &ok); Check(ok, "fixture program link");
    glUseProgram(program);
    glUniformBlockBinding(program, glGetUniformBlockIndex(program, "Color"), 2);
    GLuint vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    GLint alignment; glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &alignment);
    OpenGL::Driver driver;
    unsigned draws = 0;
    for (unsigned mode : {0, 1, 2, 3}) {
        OpenGL::WebGLSubmission::mode = mode;
        OpenGL::WebGLSubmission::counters = {};
        OpenGL::OGLStreamBuffer vertices(driver, GL_ARRAY_BUFFER, 256);
        OpenGL::OGLStreamBuffer indices(driver, GL_ELEMENT_ARRAY_BUFFER, 64);
        OpenGL::OGLStreamBuffer uniforms(driver, GL_UNIFORM_BUFFER, alignment * 4);
        for (unsigned frame = 0; frame < 512; ++frame) {
            // Reuse, intervening entries, and enough distinct entries to wrap each ring.
            const unsigned variant = frame % 32 < 16 ? frame % 3 : frame % 32;
            const float edge = 1.0f + variant * 0.01f;
            const std::array<float, 6> positions{-edge, -edge, 3 * edge, -edge, -edge, 3 * edge};
            const std::array<u16, 3> order = variant % 2 ? std::array<u16, 3>{0, 1, 2}
                                                        : std::array<u16, 3>{2, 1, 0};
            const std::array<float, 4> color{variant / 31.0f, (31 - variant) / 31.0f, 0, 1};
            const auto vertex_offset = Upload(vertices, positions);
            const auto index_offset = Upload(indices, order);
            const auto uniform_offset = Upload(uniforms, color, alignment);
            glBindBuffer(GL_ARRAY_BUFFER, vertices.GetHandle());
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices.GetHandle());
            glBindBufferRange(GL_UNIFORM_BUFFER, 2, uniforms.GetHandle(), uniform_offset,
                              sizeof(color));
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8,
                                  reinterpret_cast<void*>(vertex_offset));
            glEnableVertexAttribArray(0);
            glViewport(0, 0, 16, 16); glDisable(GL_CULL_FACE); glDisable(GL_DEPTH_TEST);
            glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glClearColor(0, 0, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
            if (frame % 2) glDrawArrays(GL_TRIANGLES, 0, 3);
            else glDrawRangeElements(GL_TRIANGLES, 0, 2, 3, GL_UNSIGNED_SHORT,
                                     reinterpret_cast<void*>(index_offset));
            std::array<u8, 4> pixel;
            glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
            Check(std::abs(int(pixel[0]) - int(color[0] * 255 + 0.5f)) <= 1 &&
                      std::abs(int(pixel[1]) - int(color[1] * 255 + 0.5f)) <= 1 &&
                      pixel[2] == 0 && pixel[3] == 255, "GPU vertex/index/uniform parity");
            Check(glGetError() == GL_NO_ERROR, "WebGL errors");
            ++draws;
        }
        if (mode & 1) for (const auto& counter : OpenGL::WebGLSubmission::counters)
            Check(counter.hits > 0 && counter.misses > 0, "real GPU cache hit and miss coverage");
        std::printf("PASS: mode %u, 512 real WebGL2 draws\n", mode);
    }
    glDeleteVertexArrays(1, &vao); glDeleteProgram(program);
    glDeleteShader(vs); glDeleteShader(fs);
    EM_ASM({ globalThis.__SUBMISSION_TEST_RESULT__ = ({passed: true, draws: $0, modes: 4}); }, draws);
    emscripten_webgl_destroy_context(context);
}
