#include <array>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include "common/logging/log.h"
#include "video_core/renderer_opengl/gl_shader_util.h"
#include "video_core/renderer_opengl/gl_state.h"

namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char*, unsigned int, const char*, fmt::string_view,
                       const fmt::format_args&) {}
void Stop() {}
}
namespace OpenGL {
OpenGLState::OpenGLState() { draw.shader_program = 71; }
OpenGLState OpenGLState::cur_state{};
}

static int shader_queries, program_queries, uniform_queries, compile_calls, link_calls;
static bool linked = true;
static GLuint bound_program = 71;
static std::vector<GLuint> block_bindings;

int main() {
    glad_glCreateShader = [](GLenum) -> GLuint { return 11; };
    glad_glGetError = []() -> GLenum { return GL_NO_ERROR; };
    glad_glShaderSource = [](GLuint, GLsizei, const GLchar* const*, const GLint*) {};
    glad_glCompileShader = [](GLuint) { ++compile_calls; };
    glad_glGetShaderiv = [](GLuint, GLenum pname, GLint* value) {
        ++shader_queries; *value = pname == GL_COMPILE_STATUS ? GL_TRUE : 0;
    };
    glad_glGetShaderInfoLog = [](GLuint, GLsizei, GLsizei*, GLchar*) { assert(false); };
    glad_glCreateProgram = []() -> GLuint { return 21; };
    glad_glAttachShader = [](GLuint, GLuint) {};
    glad_glDetachShader = [](GLuint, GLuint) {};
    glad_glLinkProgram = [](GLuint) { ++link_calls; };
    glad_glGetProgramiv = [](GLuint, GLenum pname, GLint* value) {
        ++program_queries; *value = pname == GL_LINK_STATUS && linked ? GL_TRUE : 0;
    };
    glad_glGetProgramInfoLog = [](GLuint, GLsizei, GLsizei*, GLchar*) { assert(false); };
    glad_glUseProgram = [](GLuint program) { bound_program = program; };
    glad_glGetUniformLocation = [](GLuint, const GLchar*) -> GLint { ++uniform_queries; return 0; };
    glad_glUniform1i = [](GLint, GLint) { assert(bound_program == 21); };
    glad_glGetUniformBlockIndex = [](GLuint, const GLchar*) -> GLuint { ++uniform_queries; return 0; };
    glad_glUniformBlockBinding = [](GLuint, GLuint, GLuint binding) { block_bindings.push_back(binding); };

    assert(OpenGL::LoadShader("void main(){}", GL_FRAGMENT_SHADER, "test", false) == 11);
    assert(compile_calls == 1 && shader_queries == 0);
    assert(OpenGL::LoadProgram(false, std::array<GLuint,2>{10,11}, "test", false) == 21);
    assert(link_calls == 1 && program_queries == 0 && uniform_queries == 0);
    assert(bound_program == 71);
    assert(OpenGL::FinalizeProgram(21,"test"));
    assert(program_queries == 2 && uniform_queries == 12 && bound_program == 71);
    assert((block_bindings == std::vector<GLuint>{0,1,2,3}));
    linked = false;
    assert(!OpenGL::FinalizeProgram(21,"failed"));
    assert(uniform_queries == 12 && bound_program == 71);
    linked = true;
    OpenGL::LoadShader("void main(){}", GL_FRAGMENT_SHADER, "sync");
    assert(shader_queries == 2);
    OpenGL::LoadProgram(false, std::array<GLuint,2>{10,11}, "sync");
    assert(program_queries == 6 && bound_program == 71);
    std::puts("PASS: asynchronous compile/link issue no status or uniform queries; finalization preserves GL state; failure and synchronous paths checked");
}
