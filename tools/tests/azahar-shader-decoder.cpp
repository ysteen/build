#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string_view>
#include <type_traits>
#include <vector>
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

// Exercise ShaderSetup's real serialized fields and load hook without depending on the
// frontend save format. This is not an end-to-end game save/load test.
template <bool Loading>
struct FieldArchive {
    using is_loading = std::bool_constant<Loading>;
    std::vector<u8>& bytes;
    size_t offset = 0;

    template <typename T>
    FieldArchive& operator&(T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        if constexpr (Loading) {
            Check(offset + sizeof(T) <= bytes.size(), "archive bounds");
            std::memcpy(&value, bytes.data() + offset, sizeof(T));
            offset += sizeof(T);
        } else {
            const auto* data = reinterpret_cast<const u8*>(&value);
            bytes.insert(bytes.end(), data, data + sizeof(T));
        }
        return *this;
    }
};

static Instruction Arithmetic(OpCode::Id op, std::mt19937& random) {
    Instruction instr{};
    instr.opcode.Assign(op);
    const bool mad = instr.opcode.Value().GetInfo().type == OpCode::Type::MultiplyAdd;
    const bool inverted = instr.opcode.Value().GetInfo().subtype & OpCode::Info::SrcInversed;
    if (mad) {
        instr.mad.dest.Assign(DestRegister(random() % 32));
        instr.mad.src1.Assign(SourceRegister(random() % 32));
        instr.mad.address_register_index.Assign(random() % 4);
        instr.mad.operand_desc_id.Assign(random() % 32);
        if (inverted) {
            instr.mad.src2i.Assign(SourceRegister(random() % 32));
            instr.mad.src3i.Assign(SourceRegister(random() % 128));
        } else {
            instr.mad.src2.Assign(SourceRegister(random() % 128));
            instr.mad.src3.Assign(SourceRegister(random() % 32));
        }
    } else {
        instr.common.dest.Assign(DestRegister(random() % 32));
        instr.common.address_register_index.Assign(random() % 4);
        instr.common.operand_desc_id.Assign(random() % 128);
        if (inverted) {
            instr.common.src1i.Assign(SourceRegister(random() % 32));
            instr.common.src2i.Assign(SourceRegister(random() % 128));
        } else {
            instr.common.src1.Assign(SourceRegister(random() % 128));
            instr.common.src2.Assign(SourceRegister(random() % 32));
        }
        if (op == OpCode::Id::CMP) {
            instr.common.compare_op.x.Assign(
                static_cast<Instruction::Common::CompareOpType::Op>(random() % 6));
            instr.common.compare_op.y.Assign(
                static_cast<Instruction::Common::CompareOpType::Op>(random() % 6));
        }
    }
    return instr;
}

static Instruction Simple(OpCode::Id op, unsigned destination = 0, unsigned length = 0) {
    Instruction instr{};
    instr.opcode.Assign(op);
    instr.flow_control.dest_offset.Assign(destination);
    instr.flow_control.num_instructions.Assign(length);
    return instr;
}

static void Compare(const ShaderSetup& setup, const ShaderUnit& initial, unsigned entry = 0) {
    ShaderUnit expected = initial, actual = initial;
    DebugData<false> debug;
    RunInterpreter<false, false>(setup, expected, debug, entry);
    RunInterpreter<false, true>(setup, actual, debug, entry);
    const auto compare = [](const auto& left, const auto& right) {
        for (unsigned i = 0; i < left.size(); ++i) {
            for (unsigned lane = 0; lane < 4; ++lane) {
                const float a = left[i][lane].ToFloat32(), b = right[i][lane].ToFloat32();
                Check((std::isnan(a) && std::isnan(b)) ||
                          std::bit_cast<u32>(a) == std::bit_cast<u32>(b),
                      "decoded shader register differs from reference");
            }
        }
    };
    compare(expected.input, actual.input);
    compare(expected.temporary, actual.temporary);
    compare(expected.output, actual.output);
    Check(std::equal(std::begin(expected.address_registers), std::end(expected.address_registers),
                     std::begin(actual.address_registers)), "address registers");
    Check(std::equal(std::begin(expected.conditional_code), std::end(expected.conditional_code),
                     std::begin(actual.conditional_code)), "condition codes");
}

static void CheckShaders() {
    std::mt19937 random{0xdec0de};
    ShaderSetup setup;
    setup.interpreter_cache.program = std::make_unique<DecodedProgram>();
    ShaderUnit state;
    constexpr std::array ops{OpCode::Id::ADD, OpCode::Id::MUL, OpCode::Id::MOV,
        OpCode::Id::DP3, OpCode::Id::DP4, OpCode::Id::DPH, OpCode::Id::DPHI,
        OpCode::Id::MIN, OpCode::Id::MAX, OpCode::Id::FLR, OpCode::Id::RCP,
        OpCode::Id::RSQ, OpCode::Id::SGE, OpCode::Id::SGEI, OpCode::Id::SLT,
        OpCode::Id::SLTI, OpCode::Id::CMP, OpCode::Id::MAD, OpCode::Id::MADI,
        OpCode::Id::EX2, OpCode::Id::LG2};
    constexpr std::array offsets{-200, -129, -128, -96, -1, 0, 1, 31, 95, 96, 127, 128, 512};
    constexpr std::array specials{0.f, -0.f, INFINITY, -INFINITY, NAN, -1.f, 1.f, 0.5f};
    const auto fill = [&](auto& vectors, bool special) {
        for (auto& vec : vectors) {
            for (unsigned lane = 0; lane < 4; ++lane) {
                vec[lane] = f24::FromFloat32(special ? specials[random() % specials.size()]
                    : (static_cast<int>(random() % 1024) - 512) / 64.f);
            }
        }
    };
    for (unsigned run = 0; run < 5000; ++run) {
        fill(state.input, run % 3 == 0);
        fill(state.temporary, run % 3 == 0);
        fill(state.output, run % 3 == 0);
        fill(setup.uniforms.f, run % 3 == 0);
        for (auto& offset : state.address_registers) offset = offsets[random() % offsets.size()];
        for (unsigned i = 0; i < 128; ++i) setup.UpdateSwizzleData(i, random());
        for (unsigned i = 0; i < 32; ++i)
            setup.UpdateProgramCode(i, Arithmetic(ops[random() % ops.size()], random).hex);
        setup.UpdateProgramCode(32, Simple(OpCode::Id::END).hex);
        Compare(setup, state);
        // A warm cache must observe changed uniform values without decoding again.
        fill(setup.uniforms.f, false);
        Compare(setup, state);
        // Change only a swizzle, then only an instruction after the cache has been populated.
        setup.UpdateSwizzleData(run % 128, random());
        Compare(setup, state);
        setup.UpdateProgramCode(run % 32, Arithmetic(OpCode::Id::MOV, random).hex);
        Compare(setup, state);
    }

    fill(state.input, false);
    fill(state.temporary, false);
    fill(state.output, false);
    fill(setup.uniforms.f, false);
    for (unsigned run = 0; run < 128; ++run) {
        ProgramCode code{};
        code[0] = Arithmetic(OpCode::Id::MOVA, random).hex;
        code[1] = Simple(OpCode::Id::LOOP, 5).hex;
        code[2] = Simple(run & 1 ? OpCode::Id::IFU : OpCode::Id::IFC, 4, 1).hex;
        code[3] = Arithmetic(OpCode::Id::ADD, random).hex;
        code[4] = Arithmetic(OpCode::Id::MUL, random).hex;
        code[5] = Simple(run & 2 ? OpCode::Id::BREAKC : OpCode::Id::NOP).hex;
        code[6] = Simple(run & 4 ? OpCode::Id::CALLU : OpCode::Id::CALL, 10, 2).hex;
        code[7] = Arithmetic(OpCode::Id::CMP, random).hex;
        code[8] = Simple(run & 8 ? OpCode::Id::JMPU : OpCode::Id::JMPC, 13).hex;
        code[9] = code[12] = code[14] = Simple(OpCode::Id::END).hex;
        code[10] = Arithmetic(OpCode::Id::MAD, random).hex;
        code[11] = Arithmetic(OpCode::Id::MOV, random).hex;
        code[13] = Arithmetic(OpCode::Id::DP4, random).hex;
        setup.UpdateProgramCode(code, 15);
        setup.uniforms.b[0] = run & 16;
        setup.uniforms.i[0] = {static_cast<u8>(run % 4), 0, 1, 0};
        state.conditional_code[0] = run & 32;
        state.conditional_code[1] = run & 64;
        Compare(setup, state);
    }
    // The interpreter's final-slot END guard must survive cached and uncached entry points.
    Compare(setup, state, MAX_PROGRAM_CODE_LENGTH - 1);
    Compare(setup, state, MAX_PROGRAM_CODE_LENGTH + 100);

    // Hash calculation clears independent dirty flags; decoder invalidation must not rely on them.
    setup.GetProgramCodeHash();
    setup.GetSwizzleDataHash();
    setup.UpdateProgramCode(0, Arithmetic(OpCode::Id::MOV, random).hex);
    setup.UpdateProgramCode(1, Simple(OpCode::Id::END).hex);
    Compare(setup, state);
    auto swizzles = setup.GetSwizzleData();
    for (auto& swizzle : swizzles) swizzle = random();
    setup.UpdateSwizzleData(swizzles);
    Compare(setup, state);

    std::vector<u8> saved_fields;
    FieldArchive<false> save{saved_fields};
    boost::serialization::access::serialize(save, setup, 0);
    auto* cache = setup.interpreter_cache.program.get();
    setup.UpdateProgramCode(0, Arithmetic(OpCode::Id::ADD, random).hex);
    Compare(setup, state);
    const auto changed_version = setup.interpreter_cache.version;
    FieldArchive<true> load{saved_fields};
    boost::serialization::access::serialize(load, setup, 0);
    Check(setup.interpreter_cache.program.get() == cache, "transient cache must not be serialized");
    Check(setup.interpreter_cache.version != changed_version, "state-load cache invalidation");
    Compare(setup, state);

    // Even generation wrap must not resurrect instructions from an older shader.
    setup.interpreter_cache.program->instructions[0].version = 1;
    setup.interpreter_cache.version = std::numeric_limits<u64>::max();
    setup.UpdateProgramCode(0, Arithmetic(OpCode::Id::MUL, random).hex);
    Check(setup.interpreter_cache.version == 1, "generation wrap");
    Compare(setup, state);

    ShaderSetup copied = setup;
    Check(!copied.interpreter_cache.program, "copied setup must not share mutable decode cache");
    copied.interpreter_cache.program = std::make_unique<DecodedProgram>();
    Compare(copied, state);
    copied.UpdateProgramCode(0, Arithmetic(OpCode::Id::DP4, random).hex);
    Compare(copied, state);
    Compare(setup, state);
    copied = setup;
    Check(!copied.interpreter_cache.program, "assigned setup must discard its old decode cache");
    copied.interpreter_cache.program = std::make_unique<DecodedProgram>();
    Compare(copied, state);
    std::puts("PASS: 20,000 shader comparisons, cache invalidation, relative uniforms and control flow");
}

static void CheckGeometry() {
    ShaderSetup setup;
    setup.interpreter_cache.program = std::make_unique<DecodedProgram>();
    SwizzlePattern swizzle{};
    swizzle.dest_mask.Assign(15);
    swizzle.src1_selector.Assign(0x1b);
    setup.UpdateSwizzleData(0, swizzle.hex);
    for (unsigned vertex = 0; vertex < 3; ++vertex) {
        Instruction move{};
        move.opcode.Assign(OpCode::Id::MOV);
        move.common.src1.Assign(SourceRegister::MakeInput(vertex));
        move.common.dest.Assign(DestRegister::MakeOutput(0));
        setup.UpdateProgramCode(vertex * 3, move.hex);
        Instruction emit{};
        emit.opcode.Assign(OpCode::Id::SETEMIT);
        emit.setemit.vertex_id.Assign(vertex);
        emit.setemit.prim_emit.Assign(vertex == 2);
        emit.setemit.winding.Assign(1);
        setup.UpdateProgramCode(vertex * 3 + 1, emit.hex);
        setup.UpdateProgramCode(vertex * 3 + 2, Simple(OpCode::Id::EMIT).hex);
    }
    setup.UpdateProgramCode(9, Simple(OpCode::Id::END).hex);
    std::array<std::vector<AttributeBuffer>, 2> vertices;
    std::array<unsigned, 2> winding{};
    for (unsigned path = 0; path < 2; ++path) {
        GeometryEmitter emitter{};
        Handlers handlers{
            [&](const AttributeBuffer& vertex) { vertices[path].push_back(vertex); },
            [&]() { ++winding[path]; },
        };
        emitter.handlers = &handlers;
        emitter.output_mask = 1;
        ShaderUnit state{&emitter};
        for (unsigned vertex = 0; vertex < 3; ++vertex) {
            state.input[vertex] = Common::Vec4<f24>::AssignToAll(f24::FromFloat32(vertex + 1.f));
        }
        DebugData<false> debug;
        if (path) RunInterpreter<false, true>(setup, state, debug, 0);
        else RunInterpreter<false, false>(setup, state, debug, 0);
    }
    Check(vertices[0].size() == 3 && vertices[1].size() == 3, "geometry emit count");
    Check(winding[0] == 1 && winding[1] == 1, "geometry winding");
    for (unsigned vertex = 0; vertex < 3; ++vertex) {
        for (unsigned lane = 0; lane < 4; ++lane) {
            Check(vertices[0][vertex][0][lane] == vertices[1][vertex][0][lane],
                  "geometry emitted vertex");
        }
    }
    std::puts("PASS: geometry SETEMIT/EMIT output and winding");
}

template <bool Decoded>
[[gnu::noinline]] static u32 RunBatch(const ShaderSetup& setup, unsigned count) {
    ShaderUnit state;
    DebugData<false> debug;
    u32 checksum = 0;
    for (unsigned i = 0; i < count; ++i) {
        state.input[0] = Common::Vec4<f24>::AssignToAll(
            f24::FromFloat32(0.5f + static_cast<float>(i & 255) / 256.f));
        state.temporary[0] = state.input[0];
        RunInterpreter<false, Decoded>(setup, state, debug, 0);
        checksum += std::bit_cast<u32>(state.output[0].x.ToFloat32());
    }
    asm volatile("" : "+r"(checksum) : : "memory");
    return checksum;
}

static void Benchmark(std::string_view workload) {
    ShaderSetup setup;
    setup.interpreter_cache.program = std::make_unique<DecodedProgram>();
    SwizzlePattern swizzle{};
    swizzle.dest_mask.Assign(15);
    swizzle.src1_selector.Assign(0x1b);
    swizzle.src2_selector.Assign(0x1b);
    swizzle.src3_selector.Assign(0x1b);
    setup.UpdateSwizzleData(0, swizzle.hex);
    if (workload == "swizzled" || workload == "masked") {
        constexpr std::array selectors{0x1b, 0x00, 0x55, 0xaa, 0xff, 0x6c, 0xb1, 0xe4};
        for (unsigned i = 0; i < 16; ++i) {
            swizzle.src1_selector.Assign(selectors[i % selectors.size()]);
            swizzle.src2_selector.Assign(selectors[(i + 3) % selectors.size()]);
            swizzle.negate_src1.Assign(i & 1);
            swizzle.negate_src2.Assign((i >> 1) & 1);
            swizzle.dest_mask.Assign(workload == "masked" ? i : 15);
            setup.UpdateSwizzleData(i + 1, swizzle.hex);
        }
    }
    for (auto& uniform : setup.uniforms.f)
        uniform = Common::Vec4<f24>::AssignToAll(
            f24::FromFloat32(workload == "mad" ? 1.001f : 0.5f));
    for (unsigned i = 0; i < 64; ++i) {
        Instruction instr{};
        if (workload == "mad") {
            instr.opcode.Assign(OpCode::Id::MAD);
            instr.mad.src1.Assign(SourceRegister::MakeTemporary(0));
            instr.mad.src2.Assign(SourceRegister::MakeFloat(i));
            instr.mad.src3.Assign(SourceRegister::MakeInput(0));
            instr.mad.dest.Assign(DestRegister::MakeTemporary(0));
        } else {
            const auto opcode = workload == "unary"
                ? (i % 4 == 0 ? OpCode::Id::MOV : i % 4 == 1 ? OpCode::Id::FLR
                    : i % 4 == 2 ? OpCode::Id::RCP : OpCode::Id::RSQ)
                : (i % 4 == 0 ? OpCode::Id::DP4 : i % 4 == 1 ? OpCode::Id::MUL
                    : i % 4 == 2 ? OpCode::Id::ADD : OpCode::Id::MOV);
            instr.opcode.Assign(opcode);
            instr.common.src1.Assign(workload == "unary" || opcode == OpCode::Id::MOV
                ? SourceRegister::MakeTemporary(0) : SourceRegister::MakeFloat(i));
            instr.common.src2.Assign(SourceRegister::MakeTemporary(0));
            instr.common.dest.Assign(DestRegister::MakeTemporary(0));
            if (workload == "swizzled" || workload == "masked") {
                instr.common.operand_desc_id.Assign(1 + i % 16);
            }
            if (workload == "relative") {
                instr.common.address_register_index.Assign(1);
            }
        }
        setup.UpdateProgramCode(i, instr.hex);
    }
    Instruction output{};
    output.opcode.Assign(OpCode::Id::MOV);
    output.common.src1.Assign(SourceRegister::MakeTemporary(0));
    output.common.dest.Assign(DestRegister::MakeOutput(0));
    setup.UpdateProgramCode(64, output.hex);
    setup.UpdateProgramCode(65, Simple(OpCode::Id::END).hex);
    std::array<double, 5> reference, decoded;
    for (unsigned repeat = 0; repeat < 5; ++repeat) {
        auto start = std::chrono::steady_clock::now();
        const auto a = RunBatch<false>(setup, 100000);
        reference[repeat] = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        start = std::chrono::steady_clock::now();
        const auto b = RunBatch<true>(setup, 100000);
        decoded[repeat] = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        Check(a == b, "benchmark checksum");
    }
    std::sort(reference.begin(), reference.end());
    std::sort(decoded.begin(), decoded.end());
    std::printf("shader decode (%.*s): reference=%.3f ms cached=%.3f ms\n",
                static_cast<int>(workload.size()), workload.data(), reference[2], decoded[2]);
}

int main(int argc, char** argv) {
    CheckShaders();
    CheckGeometry();
    if (argc == 1 || std::string_view(argv[1]) != "--check-only") {
        Benchmark("mixed");
        Benchmark("mad");
        Benchmark("unary");
        Benchmark("swizzled");
        Benchmark("masked");
        Benchmark("relative");
    }
}
