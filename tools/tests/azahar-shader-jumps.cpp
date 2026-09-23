#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "video_core/shader/generator/glsl_shader_decompiler.h"
#include "video_core/shader/shader_interpreter.cpp"

using namespace Pica;
using namespace Pica::Shader;
using nihstro::DestRegister;
using nihstro::Instruction;
using nihstro::OpCode;
using nihstro::SourceRegister;
using nihstro::SwizzlePattern;

static void Check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::abort();
    }
}

namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char*, unsigned int, const char*, fmt::string_view,
                       const fmt::format_args&) {
    Check(false, "unexpected shader log/assertion");
}
void Stop() {}
} // namespace Common::Log

static u32 Flow(OpCode::Id op, u32 destination = 0, u32 length = 0, u32 bool_uniform = 0) {
    Instruction instr{};
    instr.opcode.Assign(op);
    instr.flow_control.dest_offset.Assign(destination);
    instr.flow_control.num_instructions.Assign(length);
    instr.flow_control.bool_uniform_id.Assign(bool_uniform);
    if (op == OpCode::Id::JMPC) {
        instr.flow_control.op.Assign(Instruction::FlowControlType::Op::JustX);
        instr.flow_control.refx.Assign(1);
    }
    return instr.hex;
}

static u32 Arithmetic(OpCode::Id op, DestRegister destination, SourceRegister source1,
                      SourceRegister source2 = SourceRegister::MakeInput(1)) {
    Instruction instr{};
    instr.opcode.Assign(op);
    instr.common.dest.Assign(destination);
    instr.common.src1.Assign(source1);
    instr.common.src2.Assign(source2);
    if (op == OpCode::Id::CMP) {
        instr.common.compare_op.x.Assign(Instruction::Common::CompareOpType::LessThan);
        instr.common.compare_op.y.Assign(Instruction::Common::CompareOpType::LessThan);
    }
    return instr.hex;
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
        default: out += c; break;
        }
    }
    return out + '"';
}

int main(int argc, char** argv) {
    const u32 stress_blocks = argc > 1 ? static_cast<u32>(std::stoul(argv[1])) : 48;
    Check(stress_blocks > 0 && stress_blocks <= 64, "stress block count must be 1-64");
    const auto temporary = DestRegister::MakeTemporary(0);
    const auto temp_source = SourceRegister::MakeTemporary(0);
    const u32 move = Arithmetic(OpCode::Id::MOV, temporary, SourceRegister::MakeInput(0));
    const u32 add = Arithmetic(OpCode::Id::ADD, temporary, temp_source);
    const u32 add_three = Arithmetic(OpCode::Id::ADD, temporary, temp_source,
                                     SourceRegister::MakeInput(2));
    const u32 output = Arithmetic(OpCode::Id::MOV, DestRegister::MakeOutput(0), temp_source);
    const u32 end = Flow(OpCode::Id::END);
    std::vector<std::pair<std::string, std::vector<u32>>> programs{
        {"forward-jump", {move, Flow(OpCode::Id::JMPU, 3), add, output, end}},
        {"backward-jump", {move, add,
            Arithmetic(OpCode::Id::CMP, temporary, temp_source, SourceRegister::MakeInput(2)),
            Flow(OpCode::Id::JMPC, 1), output, end}},
        {"subroutine-jump", {move, Flow(OpCode::Id::CALL, 4, 3), output, end,
            Flow(OpCode::Id::JMPU, 6), add, add}},
        {"jump-into-if", {move, Flow(OpCode::Id::JMPU, 4), Flow(OpCode::Id::IFU, 6, 1, 1),
            add, add, add, add_three, output, end}},
        {"jump-in-loop", {move, Flow(OpCode::Id::LOOP, 5), Flow(OpCode::Id::JMPU, 4),
            add, add, Flow(OpCode::Id::NOP), output, end}},
        {"inverted-uniform-jump", {move, Flow(OpCode::Id::JMPU, 3, 1), add, output, end}},
        {"jump-to-empty-last-block", {move, Flow(OpCode::Id::CALL, 4, 3), output, end,
            Flow(OpCode::Id::JMPU, 6), add, Flow(OpCode::Id::NOP)}},
    };

    // Many non-empty fall-through cases caused ANGLE to duplicate the remaining
    // blocks under every case. Exercise both branch outcomes against the CPU
    // interpreter and provide a representative compile-time stress fixture.
    std::vector<u32> chained{move};
    for (u32 block = 0; block < stress_blocks; ++block) {
        const u32 offset = static_cast<u32>(chained.size());
        chained.push_back(Flow(OpCode::Id::JMPU, offset + 3, 0, block % 4));
        chained.push_back(add);
        chained.push_back(add_three);
    }
    chained.push_back(output);
    chained.push_back(end);
    programs.emplace_back("chained-forward-jumps", std::move(chained));

    std::puts("[");
    bool first = true;
    for (const auto& [name, instructions] : programs) {
        ShaderSetup setup;
        ProgramCode code{};
        std::copy(instructions.begin(), instructions.end(), code.begin());
        setup.UpdateProgramCode(code, instructions.size());
        SwizzlePattern swizzle{};
        swizzle.dest_mask.Assign(15);
        swizzle.src1_selector.Assign(0x1b);
        swizzle.src2_selector.Assign(0x1b);
        setup.UpdateSwizzleData(0, swizzle.hex);
        auto source = Generator::GLSL::DecompileProgram(setup.GetProgramCode(),
            setup.GetSwizzleData(), 0,
            [](u32 reg) { return "input" + std::to_string(reg); },
            [](u32 reg) { return "result" + std::to_string(reg); }, false);
        Check(!source.empty(), "shader must decompile");
#if defined(__EMSCRIPTEN__)
        Check(source.find("int jmp_to = ") != std::string::npos, "signed WebGL dispatcher");
        Check(source.find("uint jmp_to = ") == std::string::npos, "no unsigned WebGL dispatcher");
#else
        Check(source.find("uint jmp_to = ") != std::string::npos, "native dispatcher unchanged");
#endif
        source = R"(#version 300 es
precision highp float;
precision highp int;
layout(location=0) in vec4 input0;
layout(location=1) in vec4 input1;
layout(location=2) in vec4 input2;
layout(std140) uniform vs_pica_data { uint b; uvec4 i[4]; vec4 f[96]; } uniforms;
out vec4 result0;
)" + source + R"(
void main() { result0 = vec4(0.0); exec_shader(); gl_Position = vec4(0.0, 0.0, 0.0, 1.0); gl_PointSize = 1.0; }
)";
        if (!first) std::puts(",");
        first = false;
        std::printf("{\"name\":%s,\"source\":%s,\"cases\":[", Quote(name).c_str(),
                    Quote(source).c_str());
        for (u32 mask = 0; mask < 16; ++mask) {
            ShaderUnit state{};
            for (u32 lane = 0; lane < 4; ++lane) {
                state.input[0][lane] = f24::FromFloat32(static_cast<float>(lane));
                state.input[1][lane] = f24::FromFloat32(1.0f);
                state.input[2][lane] = f24::FromFloat32(3.0f);
            }
            for (u32 bit = 0; bit < 4; ++bit) setup.uniforms.b[bit] = mask & (1u << bit);
            setup.uniforms.i[0] = {2, 0, 1, 0};
            DebugData<false> debug;
            RunInterpreter<false, false>(setup, state, debug, 0);
            std::printf("%s{\"bools\":%u,\"expected\":[%.9g,%.9g,%.9g,%.9g]}",
                mask ? "," : "", mask, state.output[0].x.ToFloat32(),
                state.output[0].y.ToFloat32(), state.output[0].z.ToFloat32(),
                state.output[0].w.ToFloat32());
        }
        std::printf("]}");
    }
    std::puts("\n]");
    std::fprintf(stderr, "PASS: %zu jump fixtures, native interpreter reference outputs\n", programs.size());
}
