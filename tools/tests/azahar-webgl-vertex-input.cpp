#include <cassert>
#include <cstdio>
#include <vector>
#include "video_core/renderer_opengl/gl_webgl_vertex_input.h"

using Format = Pica::PipelineRegs::VertexAttributeFormat;
static void Bytes(std::span<const u8> bytes) {
    std::printf("[");
    for (std::size_t i = 0; i < bytes.size(); ++i) std::printf("%s%u", i ? "," : "", bytes[i]);
    std::printf("]");
}

int main() {
    std::array<u8, 48> output{};
    const std::array<u8, 12> zero{};
    assert(!OpenGL::ConvertWebGLVertexInput(zero, 4, 0, Format::BYTE, output));
    assert(!OpenGL::ConvertWebGLVertexInput(zero, 4, 5, Format::BYTE, output));
    assert(!OpenGL::ConvertWebGLVertexInput(zero, 4, 4, static_cast<Format>(4), output));
    assert(!OpenGL::ConvertWebGLVertexInput(zero, 0, 4, Format::BYTE, output));
    assert(!OpenGL::ConvertWebGLVertexInput(std::span{zero}.first(11), 4, 4, Format::BYTE, output));
    assert(!OpenGL::ConvertWebGLVertexInput(zero, 4, 4, Format::BYTE, std::span{output}.first(47)));
    assert(OpenGL::ConvertWebGLVertexInput({}, 0, 4, Format::FLOAT, {}));
    const std::array<u32, 4> bits{0x80000000, 0x7fc12345, 0x7f800000, 0xff800000};
    std::array<u8, 16> special{};
    std::memcpy(special.data(), bits.data(), special.size());
    assert(OpenGL::ConvertWebGLVertexInput(special, 16, 4, Format::FLOAT,
                                          std::span{output}.first(16)));
    assert(std::memcmp(special.data(), output.data(), special.size()) == 0);

    bool first = true;
    std::printf("[");
    for (u32 type = 0; type < 4; ++type) {
        const u32 width = std::array{1, 1, 2, 4}[type];
        for (u32 components = 1; components <= 4; ++components) {
            for (u32 padding : {0, 2}) {
                const u32 stride = (components + padding) * width;
                // Exercise unaligned host reads separately from the aligned GPU buffer.
                std::vector<u8> storage(1 + stride * 3, 0xcd);
                auto source = std::span{storage}.subspan(1);
                std::array<float, 12> expected{};
                for (u32 vertex = 0; vertex < 3; ++vertex) {
                    expected[vertex * 4 + 3] = 1.f;
                    for (u32 component = 0; component < components; ++component) {
                        const u32 i = (vertex + component) % 4;
                        const float value = type == 0 ? std::array<float, 4>{-128, -1, 0, 127}[i]
                            : type == 1 ? std::array<float, 4>{0, 1, 128, 255}[i]
                            : type == 2 ? std::array<float, 4>{-32768, -1, 0, 32767}[i]
                                        : std::array<float, 4>{-0.f, 0.25f, -123.75f, 3.125f}[i];
                        auto* dest = source.data() + vertex * stride + component * width;
                        if (type == 0) { const auto v = static_cast<s8>(value); std::memcpy(dest, &v, 1); }
                        if (type == 1) { const auto v = static_cast<u8>(value); std::memcpy(dest, &v, 1); }
                        if (type == 2) { const auto v = static_cast<s16>(value); std::memcpy(dest, &v, 2); }
                        if (type == 3) std::memcpy(dest, &value, 4);
                        expected[vertex * 4 + component] = value;
                    }
                }
                assert(OpenGL::ConvertWebGLVertexInput(source, stride, components,
                                                      static_cast<Format>(type), output));
                assert(std::memcmp(output.data(), expected.data(), output.size()) == 0);
                std::printf("%s{\"type\":%u,\"components\":%u,\"stride\":%u,\"source\":",
                            first ? "" : ",", type, components, stride);
                first = false;
                Bytes(source);
                std::printf(",\"converted\":");
                Bytes(output);
                std::printf("}");
            }
        }
    }
    std::printf("]\n");
}
