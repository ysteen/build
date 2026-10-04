#include <algorithm>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "types.h"

std::string save_path;
namespace Config { bool DirectBoot = true; }
namespace NDSCart
{
u8* CartROM = nullptr;
u32 CartROMSize = 0;
unsigned Loads = 0;
std::vector<u8> LoadedSave;

bool LoadROM(const u8*, u32, const char*, bool);
bool LoadROMCommon(u32 length, const char* sram, bool direct)
{
    assert(length >= 777 && CartROMSize == 1024 && direct);
    assert(std::memcmp(CartROM + 12, "CLJK", 4) == 0);
    std::ifstream input(sram, std::ios::binary);
    LoadedSave.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    assert(LoadedSave.size() == 8192);
    Loads++;
    return true;
}
}

namespace NDS
{
void Reset()
{
    delete[] NDSCart::CartROM;
    NDSCart::CartROM = nullptr;
    NDSCart::CartROMSize = 0;
}
bool LoadROM(const u8* data, u32 size, const char* sram, bool direct)
{
    return NDSCart::LoadROM(data, size, sram, direct);
}
}

void update_save_memory_option() {}
void notify_unknown_save_memory() {}

// Compiled from the patched core so a borrowed frontend pointer fails under ASan.
#include "melonds-save-restart.inc"

int main(int argc, char** argv)
{
    assert(argc == 2);
    save_path = argv[1];
    std::vector<u8> save(8192, 0x25);
    {
        std::ofstream output(save_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(save.data()), save.size());
    }
    {
        std::vector<u8> frontendROM(777, 0xFF);
        std::memcpy(frontendROM.data() + 12, "CLJK", 4);
        assert(NDS::LoadROM(frontendROM.data(), frontendROM.size(), save_path.c_str(), true));
        assert(NDSCart::CartROM != frontendROM.data());
    }
    auto* ownedROM = NDSCart::CartROM;
    assert(std::all_of(ownedROM + 777, ownedROM + 1024, [](u8 c) { return c == 0; }));
    for (u8 version = 1; version <= 3; version++)
    {
        std::fill(save.begin(), save.end(), version);
        {
            std::ofstream output(save_path, std::ios::binary);
            output.write(reinterpret_cast<const char*>(save.data()), save.size());
        }
        retro_reset();
        assert(NDSCart::CartROM == ownedROM);
        assert(NDSCart::LoadedSave == save);
    }
    assert(NDSCart::Loads == 4);
    assert(!NDSCart::LoadROM(ownedROM, 2048, save_path.c_str(), true));
    assert(!NDSCart::LoadROM(nullptr, 0, save_path.c_str(), true));
    assert(NDSCart::CartROM == ownedROM);
    NDS::Reset();
    retro_reset();
    assert(NDSCart::Loads == 4);
    std::cout << "ROM remains owned across three save reloads after frontend data is freed\n";
}
