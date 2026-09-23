// Runs actual shader-cache initialization and GPU readback in a separate WebGL context.
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <emscripten/html5.h>
#include <glad/glad.h>
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_webgl_readback.h"
#include "supported_formats.h"

namespace OpenGL { bool GLES = true; }
extern "C" void* emscripten_webgl_get_proc_address(const char*);

static void CheckGL(const char* label) {
    const auto error = glGetError();
    if (error) {
        throw std::runtime_error(std::string(label) + ": GL error " + std::to_string(error));
    }
}

int main() {
    EmscriptenWebGLContextAttributes attributes;
    emscripten_webgl_init_context_attributes(&attributes);
    attributes.majorVersion = 2;
    attributes.antialias = false;
    const auto context = emscripten_webgl_create_context("#canvas", &attributes);
    if (context <= 0) { puts("FAIL: WebGL2 context creation"); return 1; }
    emscripten_webgl_make_context_current(context);
    gladLoadGLES2Loader(emscripten_webgl_get_proc_address);
    try {
        constexpr u32 width = 256, height = 416;
        OpenGL::OGLTexture source;
        source.Create();
        auto state = OpenGL::OpenGLState::GetCurState();
        state.blend.enabled = false;
        state.texture_units[0].texture_2d = source.handle;
        state.Apply();
        glActiveTexture(GL_TEXTURE0);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, width, height);
        std::vector<u32> packed(width * height);
        for (u32 i = 0; i < packed.size(); ++i) packed[i] = i * 2654435761u;
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                        GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, packed.data());
        CheckGL("Setup");
        // Preserve pending errors, just as the full core does between cache initialization
        // and the first save. Checking/clearing here would hide the original regression.
        GetSupportedFormats();
        OpenGL::WebGLDepthStencilReadback readback;
        std::vector<u8> result(width * height * 4);
        for (int attempt = 0; attempt < 3; ++attempt) {
            try {
                readback.Download(source.handle, 0, width, height,
                                  {0, height, width, 0}, result);
                CheckGL("State restoration");
                for (size_t j = 0; j < packed.size(); ++j) {
                    const u32 actual = result[j * 4] | (u32{result[j * 4 + 1]} << 8) |
                        (u32{result[j * 4 + 2]} << 16) | (u32{result[j * 4 + 3]} << 24);
                    if (actual != packed[j]) {
                        throw std::runtime_error("Depth/stencil mismatch at pixel " + std::to_string(j));
                    }
                }
                printf("PASS: native readback attempt %d, %zu pixels\n", attempt, packed.size());
            } catch (const std::exception& error) {
                printf("FAIL: attempt %d %s\n", attempt, error.what());
            }
        }
    } catch (const std::exception& error) {
        printf("FAIL: %s\n", error.what());
    }
    return 0;
}
