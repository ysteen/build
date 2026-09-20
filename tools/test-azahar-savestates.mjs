#!/usr/bin/env node
// Compile the actual archive preflight, bounded Zstd decoder, libretro entry
// points and RetroArch container validator against small deterministic mocks.
// This does not claim to exercise a running game's CPU/GPU serialization.
import assert from 'node:assert/strict';
import { readFileSync, writeFileSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

const root = resolve(fileURLToPath(new URL('..', import.meta.url)));
const azahar = process.env.AZAHAR_SOURCE || join(root, 'compile/azahar');
const retroarch = process.env.RETROARCH_SOURCE || join(root, 'compile/RetroArch');
const output = mkdtempSync(join(tmpdir(), 'azahar-savestates-'));
const read = (base, path) => readFileSync(join(base, path), 'utf8');
const between = (source, start, end) => {
  const a = source.indexOf(start);
  const b = source.indexOf(end, a + start.length);
  assert.ok(a >= 0 && b > a, `Missing source range: ${start}`);
  return source.slice(a, b);
};
const save = read(azahar, 'src/core/savestate.cpp');
const compression = read(azahar, 'src/common/zstd_compression.cpp');
const retro = read(retroarch, 'tasks/task_save.c');
const libretro = read(azahar, 'src/citra_libretro/citra_libretro.cpp');
const makefile = read(retroarch, 'Makefile.emulatorjs');
assert.match(makefile, /findstring azahar[\s\S]*EXPORTED_FUNCTIONS \+= ,_load_state_sync/);
assert.match(makefile, /findstring azahar[^\n]+\n[\s\S]*?LDFLAGS \+= -fexceptions/);
assert.match(read(azahar, 'CMakeLists.txt'), /COMPILE_LANGUAGE:CXX>:-fexceptions/);
const header = between(save, '#pragma pack(push, 1)', 'static std::string GetSaveStatePath');
const load = between(save, 'bool System::LoadStateBuffer(', '\n} // namespace Core');
const zstd = between(compression, 'namespace Common::Compression {', '\nnamespace FileUtil {');
const stateApi = between(libretro, 'std::optional<std::vector<u8>> savestate', '\nvoid* retro_get_memory_data');
const drain = between(libretro, 'static bool DrainAsyncOperations(', 'std::optional<std::vector<u8>> savestate');
const container = between(retro, 'static bool ejs_validate_state_container', '/* Optional Azahar web API');
const saveInfo = between(retro, 'char* save_state_info(void)', '\nbool supports_states');
const loadSyncEnd = '   return result ? 1 : 0;\n}';
const loadSync = between(retro, 'int load_state_sync(', loadSyncEnd) + loadSyncEnd;
assert.equal((saveInfo.match(/core_serialize_size\(/g) || []).length, 0);
assert.equal((saveInfo.match(/content_get_serialized_data\(/g) || []).length, 1);
assert.ok(load.indexOf('DecompressDataZSTD') < load.indexOf('iarchive ia'));

const source = `
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <vector>
#define FMT_HEADER_ONLY
#include <fmt/ranges.h>
#include <zstd.h>
using u8 = uint8_t;
using u64 = uint64_t;
using s32 = int32_t;
using u64_le = uint64_t;
using u32_le = uint32_t;
#define LOG_ERROR(...) do {} while (0)
${zstd}
namespace Common { const char* g_scm_rev = "0000000000000000000000000000000000000000"; }
namespace Core {
${header}
struct System {
    enum class ResultStatus { Success, Error };
    struct KernelState {
        int pending = 0;
        bool AreAsyncOperationsPending() { return pending > 0; }
    } kernel;
    u64 title_id = 0x1234;
    bool powered = true;
    bool loaded = false;
    bool fail_save = false;
    bool drain_throw = false;
    ResultStatus drain_result = ResultStatus::Success;
    static System& GetInstance() { static System system; return system; }
    bool IsPoweredOn() { return powered; }
    bool KernelRunning() { return powered; }
    KernelState& Kernel() { return kernel; }
    ResultStatus RunLoop() {
        if (drain_throw) throw std::runtime_error("test async failure");
        --kernel.pending;
        return drain_result;
    }
    std::vector<u8> SaveStateBuffer() const {
        if (fail_save) throw std::runtime_error("test archive error");
        return {1, 2, 3};
    }
    bool LoadStateBuffer(std::vector<u8> buffer);
};
// Only the point where Boost deserialization begins is mocked. All header,
// revision and decompression checks above that point are production code.
struct iarchive {
    std::istream& stream;
    explicit iarchive(std::istream& input) : stream(input) {}
    void operator&(System& system) {
        if (stream.get() != 0x5a) throw std::runtime_error("test malformed archive");
        system.loaded = true;
    }
};
${load}
}
struct Window { bool suppressPresentation = false; } window;
struct Instance { Window* emu_window = &window; } instance;
static Instance* emu_instance = &instance;
${drain}
${stateApi}
#define CONTENT_ALIGN_SIZE(size) ((((size) + 7) & ~size_t(7)))
#define RASTATE_END_BLOCK "END "
#define RASTATE_MEM_BLOCK "MEM "
${container}
static bool supports = true;
static int serializations = 0;
static bool core_info_current_supports_savestate() { return supports; }
static void* content_get_serialized_data(size_t* size) {
    ++serializations;
    *size = 3;
    return malloc(*size);
}
${saveInfo}
using RFILE = FILE;
#define RETRO_VFS_FILE_ACCESS_READ 1
#define RETRO_VFS_FILE_ACCESS_HINT_NONE 0
static RFILE* filestream_open(const char* path, int, int) { return fopen(path, "rb"); }
static int64_t filestream_get_size(RFILE* file) {
    fseek(file, 0, SEEK_END); auto size = ftell(file); rewind(file); return size;
}
static int filestream_close(RFILE* file) { return fclose(file); }
static int64_t filestream_read(RFILE* file, void* data, int64_t size) {
    return fread(data, 1, size, file);
}
static bool deserialize_result = true;
static int deserializations = 0;
static bool content_deserialize_state(const void*, size_t) {
    ++deserializations; return deserialize_result;
}
${loadSync}
static std::vector<u8> state(std::vector<u8> payload) {
    Core::CSTHeader header{};
    header.filetype = Core::header_magic_bytes;
    header.program_id = 0x1234;
    std::vector<u8> result(sizeof(header));
    memcpy(result.data(), &header, sizeof(header));
    auto compressed = Common::Compression::CompressDataZSTDDefault(payload);
    result.insert(result.end(), compressed.begin(), compressed.end());
    return result;
}
static void expect_invalid(std::vector<u8> bytes) {
    auto& system = Core::System::GetInstance();
    system.loaded = false;
    assert(!retro_unserialize(bytes.data(), bytes.size()));
    assert(!system.loaded);
}
int main() {
    auto& system = Core::System::GetInstance();
    assert(retro_serialize_size() == 3);
    u8 small[2], valid[3];
    assert(!retro_serialize(nullptr, 3));
    assert(!retro_serialize(small, 2));
    assert(retro_serialize(valid, 3));
    assert(!retro_serialize(valid, 3));
    system.fail_save = true;
    assert(retro_serialize_size() == 0);
    system.fail_save = false;
    system.kernel.pending = 1;
    system.drain_result = Core::System::ResultStatus::Error;
    assert(retro_serialize_size() == 0);
    assert(!window.suppressPresentation);
    system.drain_result = Core::System::ResultStatus::Success;
    system.kernel.pending = 1;
    system.drain_throw = true;
    assert(retro_serialize_size() == 0);
    assert(!window.suppressPresentation);
    window.suppressPresentation = true;
    assert(retro_serialize_size() == 0);
    assert(window.suppressPresentation);
    window.suppressPresentation = false;
    system.drain_throw = false;
    auto bytes = state({0x5a, 2, 3});
    assert(retro_unserialize(bytes.data(), bytes.size()));
    assert(system.loaded);
    auto wrong_magic = bytes; wrong_magic[0] ^= 1; expect_invalid(wrong_magic);
    auto wrong_title = bytes; wrong_title[4] ^= 1; expect_invalid(wrong_title);
    auto wrong_revision = bytes; wrong_revision[12] ^= 1; expect_invalid(wrong_revision);
    expect_invalid({});
    expect_invalid(std::vector<u8>(bytes.begin(), bytes.begin() + 256));
    bytes.pop_back(); expect_invalid(bytes);
    expect_invalid(state({}));
    expect_invalid(state({0})); // C++ exception must be caught in wasm, too.
    auto compressed = Common::Compression::CompressDataZSTDDefault(std::vector<u8>(65, 0x5a));
    assert(Common::Compression::DecompressDataZSTD(compressed, 64).empty());
    assert(Common::Compression::DecompressDataZSTD(compressed, 65).size() == 65);
    // A valid Zstd frame header declaring >4 GiB must not wrap on wasm32.
    std::vector<u8> huge = {0x28,0xb5,0x2f,0xfd,0xe0,1,0,0,0,1,0,0,0};
    assert(Common::Compression::GetDecompressedSize(huge) == 0x100000001ULL);
    assert(Common::Compression::DecompressDataZSTD(huge, 512 * 1024 * 1024).empty());
    assert(!ejs_validate_state_container(nullptr, 0));
    std::vector<u8> ra = {'R','A','S','T','A','T','E',1,'M','E','M',' ',1,0,0,0,
                         42,0,0,0,0,0,0,0,'E','N','D',' ',0,0,0,0};
    assert(ejs_validate_state_container(ra.data(), ra.size()));
    for (size_t n = 0; n < ra.size(); ++n)
        assert(!ejs_validate_state_container(ra.data(), n));
    auto bad = ra; bad[7] = 2; assert(!ejs_validate_state_container(bad.data(), bad.size()));
    bad = ra; bad[12] = 0xff; bad[13] = 0xff; bad[14] = 0xff; bad[15] = 0xff;
    assert(!ejs_validate_state_container(bad.data(), bad.size()));
    bad = ra; bad[12] = 0; assert(!ejs_validate_state_container(bad.data(), bad.size()));
    bad = ra; bad.insert(bad.begin() + 24, ra.begin() + 8, ra.begin() + 24);
    assert(!ejs_validate_state_container(bad.data(), bad.size()));
    bad = ra; bad.push_back(0); assert(!ejs_validate_state_container(bad.data(), bad.size()));
    char* info = save_state_info();
    assert(serializations == 1);
    size_t size, pointer;
    assert(sscanf(info, "%zu|%zu|1", &size, &pointer) == 2);
    assert(size == 3); free(reinterpret_cast<void*>(pointer)); free(info);
    supports = false; info = save_state_info();
    assert(serializations == 1); assert(strstr(info, "||0")); free(info);
    supports = true;
    assert(load_state_sync("does-not-exist.state") == 0);
    FILE* file = fopen("fixture.state", "wb"); assert(file);
    assert(fwrite(ra.data(), 1, ra.size(), file) == ra.size()); fclose(file);
    assert(load_state_sync("fixture.state") == 1); assert(deserializations == 1);
    deserialize_result = false;
    assert(load_state_sync("fixture.state") == 0); assert(deserializations == 2);
    file = fopen("fixture.state", "wb"); assert(file);
    assert(fwrite(bad.data(), 1, bad.size(), file) == bad.size()); fclose(file);
    assert(load_state_sync("fixture.state") == 0); assert(deserializations == 2);
    puts("PASS: archive preflight, bounded Zstd, exception recovery, single serialization, sync load");
}
`;
writeFileSync(join(output, 'savestates.cpp'), source);
const run = (command, args, env = process.env) => {
  const result = spawnSync(command, args, { cwd: output, stdio: 'inherit', env });
  if (result.error) throw result.error;
  assert.equal(result.status, 0, `${command} failed`);
};
const includes = ['-I' + join(azahar, 'externals/fmt/include'), '-I' + join(azahar, 'externals/zstd/lib')];
const zstdRoot = join(azahar, 'externals/zstd/lib');
const { readdirSync } = await import('node:fs');
const zstdSources = ['common', 'compress', 'decompress'].flatMap((dir) =>
  readdirSync(join(zstdRoot, dir)).filter((name) => name.endsWith('.c')).map((name) => join(zstdRoot, dir, name)));
run(process.env.CC || 'gcc', ['-O2', '-DZSTD_DISABLE_ASM', '-r', ...zstdSources, '-o', 'zstd-native.o']);
run(process.env.CXX || 'g++', ['-std=c++20', '-O1', '-g', '-D__EMSCRIPTEN__',
  '-fsanitize=address,undefined', '-fno-omit-frame-pointer', ...includes, 'savestates.cpp',
  'zstd-native.o', '-o', 'native']);
run(join(output, 'native'), [], { ...process.env, ASAN_OPTIONS: 'detect_leaks=0' });
const emcc = process.env.EMCC || join(root, 'emsdk/upstream/emscripten/emcc');
const emxx = process.env.EMXX || join(root, 'emsdk/upstream/emscripten/em++');
run(emcc, ['-O2', '-DZSTD_DISABLE_ASM', '-r', ...zstdSources, '-o', 'zstd.o']);
run(emxx, ['-std=c++20', '-O2', '-fexceptions', '-sENVIRONMENT=node', ...includes,
  'savestates.cpp', 'zstd.o', '-o', 'wasm.js']);
run('node', ['wasm.js']);
console.log(`Artifacts: ${output}`);
