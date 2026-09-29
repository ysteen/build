#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "video_core/shader/generator/glsl_fs_shader_gen.h"
#include "video_core/shader/generator/glsl_webgl_generic.h"
#include "video_core/renderer_opengl/gl_webgl_stats.h"

#pragma push_macro("EM_JS")
#undef EM_JS
#define EM_JS(ret, name, params, ...) \
    const std::string name##Source = "function(program, previous) " #__VA_ARGS__;
#include "video_core/renderer_opengl/gl_webgl_program_bindings.h"
#pragma pop_macro("EM_JS")

namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char*, unsigned int, const char*, fmt::string_view,
                       const fmt::format_args&) { std::abort(); }
void Stop() {}
}

using Stage = Pica::TexturingRegs::TevStageConfig;
using namespace Pica::Shader::Generator::GLSL;

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

static auto Stages(Pica::RegsInternal& regs) {
    return std::array{&regs.texturing.tev_stage0, &regs.texturing.tev_stage1,
        &regs.texturing.tev_stage2, &regs.texturing.tev_stage3,
        &regs.texturing.tev_stage4, &regs.texturing.tev_stage5};
}

static Pica::RegsInternal Base() {
    Pica::RegsInternal regs{};
    regs.lighting.disable.Assign(1);
    regs.framebuffer.output_merger.alphablend_enable.Assign(1);
    regs.texturing.texture0.type.Assign(Pica::TexturingRegs::TextureConfig::Texture2D);
    for (auto* stage : Stages(regs)) {
        stage->color_source1.Assign(Stage::Source::Previous);
        stage->alpha_source1.Assign(Stage::Source::Previous);
    }
    auto& first = regs.texturing.tev_stage0;
    first.color_source1.Assign(Stage::Source::PrimaryColor);
    first.alpha_source1.Assign(Stage::Source::PrimaryColor);
    first.color_source2.Assign(Stage::Source::Constant);
    first.alpha_source2.Assign(Stage::Source::Constant);
    first.color_source3.Assign(Stage::Source::Texture0);
    first.alpha_source3.Assign(Stage::Source::Texture0);
    return regs;
}

int main() {
    std::array<double, 31> stats{};
    for (u32 i = 0; i < stats.size(); ++i) stats[i] = i + 0.25;
    OpenGL::PublishWebGLStats(stats);
    EM_ASM({
        const values = Object.values(globalThis.__AZAHAR_SHADER_CACHE__);
        if (values.length !== 31 || values.some((value, i) => i === 13 ? value !== true : value !== i + 0.25))
            throw new Error("Shader telemetry ABI mismatch");
    });
    Pica::Shader::Profile profile{};
    profile.has_minus_one_to_one_range = true;
    profile.has_blend_minmax_factor = true;
    bool first = true;
    std::printf("{\"generic\":%s,\"programBindings\":%s,\"fixtures\":[",
                Quote(GenerateWebGLGenericFragmentShader()).c_str(),
                Quote(BindWebGLProgramResourcesSource).c_str());
    const auto emit = [&](std::string label, const auto& input) {
        const Pica::Shader::FSConfig config{input};
        const auto generic = MakeWebGLGenericConfig(config, {}, profile);
        assert(generic);
        std::printf("%s{\"label\":%s,\"source\":%s,\"config\":[", first ? "" : ",\n",
                    Quote(label).c_str(), Quote(GenerateFragmentShader(config, {}, profile)).c_str());
        std::array<u32, sizeof(WebGLFSConfigData) / sizeof(u32)> words;
        std::memcpy(words.data(), &*generic, sizeof(words));
        for (std::size_t i = 0; i < words.size(); ++i)
            std::printf("%s%u", i ? "," : "", words[i]);
        std::printf("]}");
        first = false;
    };
    for (u32 op = 0; op < 10; ++op) {
        for (u32 scale = 0; scale < 4; ++scale) {
            auto regs = Base();
            auto& stage = regs.texturing.tev_stage0;
            stage.color_op.Assign(static_cast<Stage::Operation>(op));
            stage.alpha_op.Assign(static_cast<Stage::Operation>(op == 6 || op == 7 ? 4 : op));
            stage.color_scale.Assign(scale);
            stage.alpha_scale.Assign(3 - scale);
            emit("operation-" + std::to_string(op) + "-scale-" + std::to_string(scale), regs);
        }
    }
    u32 modifier_index = 0;
    for (u32 modifier : {0, 1, 2, 3, 4, 5, 8, 9, 12, 13}) {
        auto regs = Base();
        regs.texturing.tev_stage0.color_modifier1.Assign(static_cast<Stage::ColorModifier>(modifier));
        regs.texturing.tev_stage0.alpha_modifier1.Assign(static_cast<Stage::AlphaModifier>(modifier_index++ % 8));
        emit("modifier-" + std::to_string(modifier), regs);
    }
    for (u32 source : {0, 1, 2, 3, 4, 5, 6, 13, 14, 15}) {
        auto regs = Base();
        auto& stage = regs.texturing.tev_stage0;
        stage.color_source1.Assign(static_cast<Stage::Source>(source));
        stage.alpha_source1.Assign(static_cast<Stage::Source>(source));
        stage.color_source3.Assign(Stage::Source::Constant);
        stage.alpha_source3.Assign(Stage::Source::Constant);
        stage.color_modifier1.Assign(Stage::ColorModifier::OneMinusSourceColor);
        stage.alpha_modifier1.Assign(Stage::AlphaModifier::OneMinusSourceAlpha);
        if (source == 15) {
            const Pica::Shader::FSConfig config{regs};
            const Stage stored = config.texture.tev_stages[0];
            assert(stored.color_source3 == Stage::Source::Constant);
            assert(stored.alpha_source3 == Stage::Source::Constant);
        }
        emit("source-" + std::to_string(source), regs);
    }
    for (u32 texture : {0, 3, 5}) {
        for (u32 borders = 0; borders < 4; ++borders) {
            for (u32 tc2 = 0; tc2 < 2; ++tc2) {
                auto regs = Base();
                regs.texturing.texture0.type.Assign(static_cast<Pica::TexturingRegs::TextureConfig::TextureType>(texture));
                regs.texturing.main_config.texture2_use_coord1.Assign(tc2);
                for (auto* tex : {&regs.texturing.texture0, &regs.texturing.texture1,
                                  &regs.texturing.texture2}) {
                    tex->wrap_s.Assign(static_cast<Pica::TexturingRegs::TextureConfig::WrapMode>((borders & 1) ? 1 : 2));
                    tex->wrap_t.Assign(static_cast<Pica::TexturingRegs::TextureConfig::WrapMode>((borders & 2) ? 1 : 2));
                }
                auto& stage = regs.texturing.tev_stage0;
                stage.color_source1.Assign(Stage::Source::Texture0);
                stage.color_source2.Assign(Stage::Source::Texture1);
                stage.color_source3.Assign(Stage::Source::Texture2);
                stage.alpha_source1.Assign(Stage::Source::Texture0);
                stage.alpha_source2.Assign(Stage::Source::Texture1);
                stage.alpha_source3.Assign(Stage::Source::Texture2);
                stage.color_op.Assign(Stage::Operation::Lerp);
                stage.alpha_op.Assign(Stage::Operation::Lerp);
                emit("textures-" + std::to_string(texture) + "-border-" + std::to_string(borders) + "-tc2-" + std::to_string(tc2), regs);
            }
        }
    }
    for (u32 mask : {0, 1, 2, 4, 8, 15, 0x10, 0x20, 0x40, 0x80, 0xF0, 0xFF}) {
        auto regs = Base();
        regs.texturing.tev_combiner_buffer_input.update_mask_rgb.Assign(mask & 15);
        regs.texturing.tev_combiner_buffer_input.update_mask_a.Assign(mask >> 4);
        u32 i = 0;
        for (auto* stage : Stages(regs)) {
            stage->color_source1.Assign(i == 0 ? Stage::Source::PrimaryColor : Stage::Source::PreviousBuffer);
            stage->alpha_source1.Assign(i == 0 ? Stage::Source::PrimaryColor : Stage::Source::PreviousBuffer);
            stage->color_source2.Assign(Stage::Source::Constant);
            stage->alpha_source2.Assign(Stage::Source::Constant);
            stage->color_op.Assign(Stage::Operation::Add);
            stage->alpha_op.Assign(Stage::Operation::Modulate);
            ++i;
        }
        emit("buffer-delay-" + std::to_string(mask), regs);
    }
    for (u32 depth = 0; depth < 2; ++depth) {
        for (u32 fog : {0, 5}) {
            for (u32 flip = 0; flip < 2; ++flip) {
                auto regs = Base();
                regs.rasterizer.depthmap_enable.Assign(static_cast<Pica::RasterizerRegs::DepthBuffering>(depth));
                regs.texturing.fog_mode.Assign(static_cast<Pica::TexturingRegs::FogMode>(fog));
                regs.texturing.fog_flip.Assign(flip);
                emit("depth-" + std::to_string(depth) + "-fog-" + std::to_string(fog) + "-flip-" + std::to_string(flip), regs);
            }
        }
    }
    for (u32 logic : {0, 3, 4, 6}) {
        auto regs = Base();
        regs.framebuffer.output_merger.alphablend_enable.Assign(0);
        regs.framebuffer.output_merger.logic_op.Assign(static_cast<Pica::FramebufferRegs::LogicOp>(logic));
        emit("logic-" + std::to_string(logic), regs);
    }
    // Exercise the fallback against the existing specialized lighting generator.
    const auto lit = [&] {
        auto regs = Base();
        regs.texturing.tev_stage0.color_source1.Assign(Stage::Source::PrimaryFragmentColor);
        regs.texturing.tev_stage0.color_source2.Assign(Stage::Source::SecondaryFragmentColor);
        regs.texturing.tev_stage0.color_op.Assign(Stage::Operation::Add);
        regs.texturing.tev_stage0.alpha_source1.Assign(Stage::Source::PrimaryFragmentColor);
        Pica::Shader::FSConfig config{regs};
        auto& light = config.lighting;
        light.enable.Assign(1); light.src_num.Assign(2);
        light.config.Assign(Pica::LightingRegs::LightingConfig::Config7);
        light.enable_primary_alpha.Assign(1); light.enable_secondary_alpha.Assign(1);
        for (u32 i = 0; i < 8; ++i) {
            light.lights[i].num.Assign((i + 3) % 8);
            light.lights[i].directional.Assign(i % 2);
            light.lights[i].two_sided_diffuse.Assign(i % 2);
        }
        for (auto* lut : {&light.lut_d0, &light.lut_d1, &light.lut_fr, &light.lut_rr,
                         &light.lut_rg, &light.lut_rb, &light.lut_sp}) {
            lut->enable.Assign(1); lut->abs_input.Assign(1);
            lut->type.Assign(Pica::LightingRegs::LightingLutInput::NH);
            lut->SetScale(0.5f);
        }
        return config;
    };
    for (u32 mode : {0, 1, 2, 3, 4, 5, 6, 8}) {
        for (u32 input = 0; input <= 5; ++input) {
            auto config = lit(); auto& light = config.lighting;
            light.config.Assign(static_cast<Pica::LightingRegs::LightingConfig>(mode));
            light.src_num.Assign(1 + input % 3);
            light.clamp_highlights.Assign(input % 2);
            for (auto* lut : {&light.lut_d0, &light.lut_d1, &light.lut_fr, &light.lut_rr,
                             &light.lut_rg, &light.lut_rb, &light.lut_sp}) {
                lut->type.Assign(static_cast<Pica::LightingRegs::LightingLutInput>(input));
                lut->abs_input.Assign(input % 2);
            }
            for (auto& slot : light.lights) {
                slot.dist_atten_enable.Assign(input % 2);
                slot.spot_atten_enable.Assign(1);
                slot.geometric_factor_0.Assign(input % 2);
                slot.geometric_factor_1.Assign((input + 1) % 2);
            }
            emit("lighting-mode-" + std::to_string(mode) + "-input-" + std::to_string(input), config);
        }
    }
    for (u32 bump = 0; bump < 3; ++bump) {
        for (u32 texture = 0; texture < 4; ++texture) {
            auto config = lit(); auto& light = config.lighting;
            light.bump_mode.Assign(static_cast<Pica::LightingRegs::LightingBumpMode>(bump));
            light.bump_selector.Assign(texture); light.bump_renorm.Assign(texture % 2);
            light.enable_shadow.Assign(1); light.shadow_selector.Assign(texture);
            light.shadow_primary.Assign(1); light.shadow_secondary.Assign(1);
            light.shadow_alpha.Assign(1); light.shadow_invert.Assign(texture % 2);
            for (auto& slot : light.lights) slot.shadow_enable.Assign(1);
            emit("lighting-bump-" + std::to_string(bump) + "-shadow-" + std::to_string(texture), config);
        }
    }
    for (u32 count : {0, 1, 8}) {
        auto config = lit(); config.lighting.src_num.Assign(count);
        for (auto* lut : {&config.lighting.lut_d0, &config.lighting.lut_d1,
                         &config.lighting.lut_fr, &config.lighting.lut_rr,
                         &config.lighting.lut_rg, &config.lighting.lut_rb}) lut->enable.Assign(0);
        emit("lighting-count-" + std::to_string(count) + "-no-luts", config);
    }
    u32 rejected = 0;
    const auto reject = [&](const Pica::Shader::FSConfig& config, Pica::Shader::UserConfig user = {}) {
        assert(!MakeWebGLGenericConfig(config, user, profile));
        ++rejected;
    };
    auto base = Base();
    for (u32 type : {1, 2, 4}) {
        auto config = Pica::Shader::FSConfig{base};
        config.texture.texture0_type.Assign(static_cast<Pica::TexturingRegs::TextureConfig::TextureType>(type));
        reject(config);
    }
    auto config = Pica::Shader::FSConfig{base};
    config.lighting.enable.Assign(1); config.lighting.src_num.Assign(9); reject(config);
    config = Pica::Shader::FSConfig{base};
    config.proctex.enable.Assign(1); reject(config);
    config = Pica::Shader::FSConfig{base};
    config.framebuffer.shadow_rendering.Assign(1); reject(config);
    config = Pica::Shader::FSConfig{base};
    config.texture.fog_mode.Assign(Pica::TexturingRegs::FogMode::Gas); reject(config);
    reject(Pica::Shader::FSConfig{base}, Pica::Shader::UserConfig{1});
    std::printf("],\"unsupportedRejected\":%u}\n", rejected);
}
