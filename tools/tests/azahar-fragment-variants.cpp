#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "video_core/shader/generator/glsl_fs_shader_gen.h"
#include "video_core/shader/generator/shader_uniforms.h"

namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char*, unsigned int, const char*, fmt::string_view,
                       const fmt::format_args&) { std::abort(); }
void Stop() {}
}

static std::string Quote(std::string_view value) {
    std::string out = "\"";
    for (char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c;
        }
    }
    return out + '"';
}

int main() {
    using Stage = Pica::TexturingRegs::TevStageConfig;
    using Operation = Stage::Operation;
    std::puts("[");
    bool first = true;
    for (u32 op = 0; op < 10; ++op) {
        for (u32 alpha = 0; alpha < 8; ++alpha) {
            for (u32 scissor : {0, 1, 3}) {
                for (u32 variant = 0; variant < 2; ++variant) {
                    Pica::RegsInternal regs{};
                    regs.lighting.disable.Assign(1);
                    regs.texturing.texture0.type.Assign(Pica::TexturingRegs::TextureConfig::Disabled);
                    regs.framebuffer.output_merger.alphablend_enable.Assign(1);
                    regs.framebuffer.output_merger.alpha_test.enable.Assign(1);
                    regs.framebuffer.output_merger.alpha_test.func.Assign(
                        static_cast<Pica::FramebufferRegs::CompareFunc>(alpha));
                    regs.rasterizer.scissor_test.mode.Assign(
                        static_cast<Pica::RasterizerRegs::ScissorMode>(scissor));
                    const std::array stages{&regs.texturing.tev_stage0, &regs.texturing.tev_stage1,
                        &regs.texturing.tev_stage2, &regs.texturing.tev_stage3,
                        &regs.texturing.tev_stage4, &regs.texturing.tev_stage5};
                    for (auto* stage : stages) {
                        stage->color_source1.Assign(Stage::Source::Previous);
                        stage->alpha_source1.Assign(Stage::Source::Previous);
                    }
                    auto& stage = regs.texturing.tev_stage0;
                    stage.color_op.Assign(static_cast<Operation>(op));
                    stage.alpha_op.Assign(static_cast<Operation>(op == 6 || op == 7 ? 2 : op));
                    stage.color_source1.Assign(Stage::Source::PrimaryColor);
                    stage.alpha_source1.Assign(Stage::Source::PrimaryColor);
                    stage.color_source2.Assign(Stage::Source::Constant);
                    stage.alpha_source2.Assign(Stage::Source::Constant);
                    stage.color_source3.Assign(Stage::Source::PreviousBuffer);
                    stage.alpha_source3.Assign(Stage::Source::PreviousBuffer);
                    if (variant) {
                        if (op == 0) {
                            stage.color_source2.Assign(Stage::Source::Texture2);
                            stage.alpha_source2.Assign(Stage::Source::Texture2);
                        }
                        if (op != 4 && op != 8 && op != 9) {
                            stage.color_source3.Assign(Stage::Source::Texture1);
                            stage.alpha_source3.Assign(Stage::Source::Texture1);
                        }
                    }
                    Pica::Shader::Profile profile{};
                    profile.has_minus_one_to_one_range = true;
                    profile.has_custom_border_color = false;
                    profile.has_blend_minmax_factor = true;
                    const auto source = Pica::Shader::Generator::GLSL::GenerateFragmentShader(
                        Pica::Shader::FSConfig{regs}, {}, profile);
                    std::printf("%s{\"op\":%u,\"alpha\":%u,\"scissor\":%u,\"variant\":%u,\"source\":%s}",
                        first ? "" : ",\n", op, alpha, scissor, variant, Quote(source).c_str());
                    first = false;
                }
            }
        }
    }
    std::puts("\n]");
}
