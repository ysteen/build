#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#include "types.h"

// Storage is real; only the platform and unrelated cartridge base are stubbed.
namespace Platform
{
struct Thread {};
struct Mutex {};
unsigned WriteOpens = 0;
FILE* OpenFile(const char* path, const char* mode)
{
    if (std::strchr(mode, 'w')) WriteOpens++;
    return std::fopen(path, mode);
}
void Mutex_Free(Mutex*) {}
void Mutex_Lock(Mutex*) {}
void Mutex_Unlock(Mutex*) {}
}

class Savestate
{
public:
    bool Saving;
    u32 VersionMajor = 9;
    u32 VersionMinor = 1;
    std::vector<u8> Data;
    size_t Position = 0;

    explicit Savestate(bool saving) : Saving(saving) {}
    explicit Savestate(const std::vector<u8>& data) : Saving(false), Data(data) {}
    void Var8(u8* value) { VarArray(value, sizeof(*value)); }
    void Var16(u16* value) { VarArray(value, sizeof(*value)); }
    void Var32(u32* value) { VarArray(value, sizeof(*value)); }
    void Var64(u64* value) { VarArray(value, sizeof(*value)); }
    void Bool32(bool* value)
    {
        u32 encoded = *value ? 1 : 0;
        Var32(&encoded);
        if (!Saving) *value = encoded != 0;
    }
    bool IsAtleastVersion(u32 major, u32 minor) const
    {
        return major < VersionMajor || (major == VersionMajor && minor <= VersionMinor);
    }
    void VarArray(void* data, u32 length)
    {
        if (Saving)
        {
            const auto* bytes = static_cast<const u8*>(data);
            Data.insert(Data.end(), bytes, bytes + length);
        }
        else
        {
            assert(Position + length <= Data.size());
            std::memcpy(data, Data.data() + Position, length);
            Position += length;
        }
    }
};

#include "NDSCart.h"
#include "NDSCart_SaveDetection.h"
#include "NDSCart_SRAMManager.h"

namespace NDSCart
{
#include "melonds-save-auto-globals.inc"

CartCommon::CartCommon(u8* rom, u32 length, u32 chipid)
    : ROM(rom), ROMLength(length), ChipID(chipid), IsDSi(false), DSiMode(false),
      DSiBase(0), CmdEncMode(0), DataEncMode(0) {}
CartCommon::~CartCommon() {}
void CartCommon::Reset() {}
void CartCommon::SetupDirectBoot() {}
void CartCommon::DoSavestate(Savestate*) {}
void CartCommon::LoadSave(const char*, u32) {}
void CartCommon::RelocateSave(const char*, bool) {}
int CartCommon::ImportSRAM(const u8*, u32) { return 0; }
void CartCommon::FlushSRAMFile() {}
int CartCommon::ROMCommandStart(u8*, u8*, u32) { return 0; }
void CartCommon::ROMCommandFinish(u8*, u8*, u32) {}
u8 CartCommon::SPIWrite(u8, u32, bool) { return 0; }
void CartCommon::ReadROM(u32, u32, u8*, u32) {}
void CartCommon::SetIRQ() {}

#include "melonds-save-auto-cart.inc"
}

#include "melonds-save-auto-manager.inc"

class TestCart : public NDSCart::CartRetail
{
public:
    TestCart() : CartRetail(nullptr, 0, 0) {}
    const u8* Data() const { return SRAM; }
    u32 Length() const { return SRAMLength; }
    u32 Type() const { return SRAMType; }
    bool Automatic() const { return SRAMAuto; }
    bool Confirmed() const { return SRAMDetector.Confirmed; }
    bool Changed() const { return SRAMDetector.Changed; }
    u32 AddressBytes() const { return SRAMDetector.AddressBytes; }
    bool Flash() const { return SRAMDetector.Flash; }
    void SetByte(u32 address, u8 value)
    {
        assert(address < SRAMLength);
        SRAM[address] = value;
    }
    std::vector<u8> Bytes() const { return {SRAM, SRAM + SRAMLength}; }
};

std::string TestDirectory;

std::string Path(const std::string& name)
{
    return TestDirectory + "/" + name + ".sav";
}

void WriteFile(const std::string& path, const std::vector<u8>& bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    assert(output.good());
}

std::vector<u8> ReadFile(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    assert(input);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<u8> Transaction(TestCart& cart, u8 command, const std::vector<u8>& payload)
{
    std::vector<u8> result;
    cart.SPIWrite(command, 0, payload.empty());
    for (size_t index = 0; index < payload.size(); index++)
        result.push_back(cart.SPIWrite(payload[index], index + 1, index + 1 == payload.size()));
    cart.SPIEndTransaction();
    return result;
}

std::vector<u8> Address(u32 address, unsigned length)
{
    std::vector<u8> result;
    while (length > 0)
    {
        length--;
        result.push_back(static_cast<u8>(address >> (length * 8)));
    }
    return result;
}

void WriteByte(TestCart& cart, u32 address, unsigned addressBytes, u8 value, bool flash)
{
    Transaction(cart, 0x06, {});
    auto payload = Address(address, addressBytes);
    payload.push_back(value);
    Transaction(cart, flash ? 0x0A : 0x02, payload);
}

u8 ReadByte(TestCart& cart, u32 address, unsigned addressBytes)
{
    auto payload = Address(address, addressBytes);
    payload.push_back(0);
    return Transaction(cart, 0x03, payload).back();
}

void Prepare(bool unknown)
{
    NDSCart::SaveMemoryUnknown = unknown;
    NDSCart::AutoSaveAddressBytes = 0;
    NDSCart::AutoSaveFlash = false;
    NDSCart::AutoSaveRestart = false;
}

void CheckKnownSaveTypes()
{
    Prepare(false);
    for (u32 type = 1; type <= 7; type++)
    {
        const auto path = Path("known-" + std::to_string(type));
        std::vector<u8> bytes(NDSCart::SaveMemorySize(type), 0xFF);
        bytes[19] = static_cast<u8>(type);
        WriteFile(path, bytes);
        TestCart cart;
        cart.LoadSave(path.c_str(), type);
        cart.Reset();
        assert(cart.Bytes() == bytes);
        assert(!NDSCart_SRAMManager::NeedsFlush());
        const unsigned addressBytes = type == 1 ? 1 : type <= 3 ? 2 : 3;
        assert(ReadByte(cart, 19, addressBytes) == type);
        WriteByte(cart, 20, addressBytes, 0x5A, type >= 5);
        assert(ReadByte(cart, 20, addressBytes) == 0x5A);
        cart.FlushSRAMFile();
        assert(NDSCart_SRAMManager::NeedsFlush());
        NDSCart_SRAMManager::FlushSecondaryBuffer();
        bytes[20] = 0x5A;
        assert(ReadFile(path) == bytes);
        assert(!NDSCart::TakeSaveDetectionRestart());
    }
}

void CheckKnownSavestateResize()
{
    Prepare(false);
    const auto path = Path("known-state");
    std::vector<u8> bytes(8192, 0x3A);
    WriteFile(path, bytes);
    TestCart cart;
    cart.LoadSave(path.c_str(), 2);
    cart.Reset();
    Savestate saved(true);
    cart.DoSavestate(&saved);
    cart.LoadSave(path.c_str(), 3);
    assert(cart.Length() == 65536);
    Savestate restored(saved.Data);
    cart.DoSavestate(&restored);
    assert(cart.Bytes() == bytes);
    NDSCart_SRAMManager::RequestFlush();
    NDSCart_SRAMManager::FlushSecondaryBuffer();
    assert(ReadFile(path) == bytes);
}

void CheckKnownEmptySaveReload()
{
    Prepare(false);
    const auto path = Path("known-empty");
    WriteFile(path, {});
    TestCart cart;
    cart.LoadSave(path.c_str(), 2);
    cart.Reset();
    Savestate saved(true);
    cart.DoSavestate(&saved);
    cart.LoadSave(path.c_str(), 0);
    assert(cart.Length() == 0 && cart.Data() == nullptr);
    Savestate restored(saved.Data);
    cart.DoSavestate(&restored);
    assert(cart.Length() == 8192);
    NDSCart_SRAMManager::FlushSecondaryBuffer();
    assert(ReadFile(path).size() == 8192);
    cart.LoadSave(path.c_str(), 0);
    assert(cart.Length() == 0 && cart.Data() == nullptr);
}

void ConfirmAddress(TestCart& cart, unsigned addressBytes)
{
    ReadByte(cart, 0, addressBytes);
    ReadByte(cart, 0, addressBytes);
    assert(cart.Confirmed());
    assert(cart.AddressBytes() == addressBytes);
}

void CheckUnknownPaddedInput()
{
    Prepare(true);
    const auto path = Path("unknown-padded");
    std::vector<u8> bytes(512 * 1024, 0xFF);
    bytes[0] = 0x41;
    bytes[65535] = 0x42;
    WriteFile(path, bytes);
    TestCart cart;
    cart.LoadSave(path.c_str(), 2);
    cart.Reset();
    assert(cart.Automatic() && !cart.Confirmed());
    assert(cart.Bytes() == bytes);
    assert(cart.Length() == 512 * 1024);
    const unsigned writes = Platform::WriteOpens;
    WriteByte(cart, 10, 2, 0x51, false);
    cart.FlushSRAMFile();
    assert(!NDSCart_SRAMManager::NeedsFlush());
    NDSCart_SRAMManager::FlushSecondaryBuffer();
    assert(Platform::WriteOpens == writes);
    assert(ReadFile(path) == bytes);

    const auto relocated = Path("unknown-padded-unconfirmed-relocate");
    cart.RelocateSave(relocated.c_str(), true);
    assert(Platform::WriteOpens == writes);
    ConfirmAddress(cart, 2);
    assert(!NDSCart::TakeSaveDetectionRestart());
    cart.FlushSRAMFile();
    NDSCart_SRAMManager::FlushSecondaryBuffer();
    bytes[10] = 0x51;
    assert(cart.Bytes() == bytes);
    assert(ReadFile(path) == bytes);
}

void CheckUnknownAddressWidths()
{
    for (unsigned addressBytes = 1; addressBytes <= 3; addressBytes++)
    {
        Prepare(true);
        const auto path = Path("unknown-address-" + std::to_string(addressBytes));
        const std::vector<u8> original(512, 0xFF);
        WriteFile(path, original);
        TestCart cart;
        cart.LoadSave(path.c_str(), 2);
        cart.Reset();
        assert(!cart.Confirmed() && cart.AddressBytes() == 2);
        ReadByte(cart, 0, addressBytes);
        assert(!cart.Confirmed());
        assert(!NDSCart::TakeSaveDetectionRestart());
        ReadByte(cart, 0, addressBytes);
        assert(cart.Confirmed() && cart.AddressBytes() == addressBytes);
        assert(NDSCart::TakeSaveDetectionRestart() == (addressBytes != 2));
        assert(!NDSCart::TakeSaveDetectionRestart());
        cart.FlushSRAMFile();
        NDSCart_SRAMManager::FlushSecondaryBuffer();
        assert(ReadFile(path) == original);
        if (addressBytes != 2)
        {
            cart.LoadSave(path.c_str(), 2);
            cart.Reset();
            assert(cart.Confirmed() && cart.AddressBytes() == addressBytes);
            assert(cart.Bytes() == original);
        }
        const u32 address = addressBytes == 1 ? 12 : addressBytes == 2 ? 9000 : 70000;
        const u32 expectedLength = addressBytes == 1 ? 512 : addressBytes == 2 ? 65536 : 131072;
        WriteByte(cart, address, addressBytes, 0x6A, false);
        assert(cart.Length() == expectedLength);
        assert(ReadByte(cart, address, addressBytes) == 0x6A);
        cart.FlushSRAMFile();
        NDSCart_SRAMManager::FlushSecondaryBuffer();
        auto saved = ReadFile(path);
        assert(saved.size() == expectedLength && saved[address] == 0x6A);
        assert(std::all_of(saved.begin(), saved.begin() + address, [](u8 byte) { return byte == 0xFF; }));
        if (addressBytes == 1)
        {
            Transaction(cart, 0x06, {});
            Transaction(cart, 0x0A, {0x2A, 0x6B});
            assert(Transaction(cart, 0x0B, {0x2A, 0}).back() == 0x6B);
            cart.FlushSRAMFile();
            NDSCart_SRAMManager::FlushSecondaryBuffer();
            assert(ReadFile(path)[0x12A] == 0x6B);
        }
    }
}

void CheckHeldChipSelect()
{
    Prepare(true);
    const auto path = Path("unknown-held-cs");
    WriteFile(path, std::vector<u8>(512, 0xFF));
    TestCart cart;
    cart.LoadSave(path.c_str(), 2);
    cart.Reset();
    ConfirmAddress(cart, 2);
    Transaction(cart, 0x06, {});
    cart.SPIWrite(0x02, 0, false);
    cart.SPIWrite(0, 1, false);
    cart.SPIWrite(17, 2, false);
    cart.SPIWrite(0x77, 3, false);
    cart.FlushSRAMFile();
    assert(!NDSCart_SRAMManager::NeedsFlush());
    cart.SPIEndTransaction();
    cart.FlushSRAMFile();
    assert(NDSCart_SRAMManager::NeedsFlush());
    NDSCart_SRAMManager::FlushSecondaryBuffer();
    assert(ReadFile(path)[17] == 0x77);
}

void CheckPendingSnapshotSurvivesGrowth()
{
    Prepare(true);
    const auto path = Path("unknown-growth-snapshot");
    WriteFile(path, std::vector<u8>(512, 0xFF));
    TestCart cart;
    cart.LoadSave(path.c_str(), 2);
    cart.Reset();
    ConfirmAddress(cart, 2);
    WriteByte(cart, 17, 2, 0x71, false);
    cart.FlushSRAMFile();
    assert(NDSCart_SRAMManager::NeedsFlush());
    WriteByte(cart, 9000, 2, 0x72, false);
    assert(cart.Length() == 65536);
    NDSCart_SRAMManager::FlushSecondaryBuffer();
    auto saved = ReadFile(path);
    assert(saved.size() == 512 && saved[17] == 0x71);
    cart.FlushSRAMFile();
    NDSCart_SRAMManager::FlushSecondaryBuffer();
    saved = ReadFile(path);
    assert(saved.size() == 65536 && saved[17] == 0x71 && saved[9000] == 0x72);
    std::fill(saved.begin(), saved.end(), 0x29);
    NDSCart_SRAMManager::UpdateBuffer(saved.data(), saved.size());
    assert(cart.Bytes() == saved);
    NDSCart_SRAMManager::RequestFlush();
    NDSCart_SRAMManager::FlushSecondaryBuffer();
    assert(ReadFile(path) == saved);
}

void CheckUpdateBufferAfterGrowth()
{
    Prepare(true);
    const auto path = Path("unknown-growth-update");
    WriteFile(path, std::vector<u8>(512, 0xFF));
    TestCart cart;
    cart.LoadSave(path.c_str(), 2);
    cart.Reset();
    ConfirmAddress(cart, 2);
    WriteByte(cart, 9000, 2, 0x27, false);
    assert(cart.Length() == 65536);
    std::vector<u8> replacement(65536, 0x2B);
    NDSCart_SRAMManager::UpdateBuffer(replacement.data(), replacement.size());
    assert(cart.Bytes() == replacement);
    NDSCart_SRAMManager::RequestFlush();
    NDSCart_SRAMManager::FlushSecondaryBuffer();
    assert(ReadFile(path) == replacement);
}

void CheckAmbiguousTrafficDoesNotPersist()
{
    Prepare(true);
    const auto path = Path("unknown-ambiguous");
    const std::vector<u8> original(512, 0xFF);
    WriteFile(path, original);
    TestCart cart;
    cart.LoadSave(path.c_str(), 2);
    cart.Reset();
    WriteByte(cart, 10, 2, 0x18, false);
    for (unsigned repeat = 0; repeat < 4; repeat++)
    {
        Transaction(cart, 0x05, {0});
        Transaction(cart, 0x03, {0, 0, 0, 0, 0, 0, 0, 0});
        ReadByte(cart, 0, repeat % 2 + 1);
    }
    assert(!cart.Confirmed());
    assert(!NDSCart::TakeSaveDetectionRestart());
    cart.FlushSRAMFile();
    NDSCart_SRAMManager::FlushSecondaryBuffer();
    assert(ReadFile(path) == original);
}

void CheckUnknownSavestateDiscardsPendingFlush()
{
    Prepare(true);
    const auto path = Path("unknown-state");
    const std::vector<u8> original(512, 0xFF);
    WriteFile(path, original);
    TestCart cart;
    cart.LoadSave(path.c_str(), 2);
    cart.Reset();
    Savestate unconfirmed(true);
    cart.DoSavestate(&unconfirmed);
    ConfirmAddress(cart, 2);
    WriteByte(cart, 9000, 2, 0x3B, false);
    cart.FlushSRAMFile();
    assert(NDSCart_SRAMManager::NeedsFlush());
    assert(cart.Length() == 65536);
    Savestate restored(unconfirmed.Data);
    cart.DoSavestate(&restored);
    assert(cart.Length() == 512 && !cart.Confirmed());
    assert(cart.Bytes() == original);
    assert(!NDSCart_SRAMManager::NeedsFlush());
    NDSCart_SRAMManager::FlushSecondaryBuffer();
    assert(ReadFile(path) == original);
    ConfirmAddress(cart, 2);
    WriteByte(cart, 20, 2, 0x3C, false);
    cart.FlushSRAMFile();
    NDSCart_SRAMManager::FlushSecondaryBuffer();
    auto saved = ReadFile(path);
    assert(saved.size() == 512 && saved[20] == 0x3C);
}

void CheckUnknownFlashRestart()
{
    Prepare(true);
    const auto path = Path("unknown-flash");
    const std::vector<u8> original(512, 0xFF);
    WriteFile(path, original);
    TestCart cart;
    cart.LoadSave(path.c_str(), 2);
    cart.Reset();
    Transaction(cart, 0x06, {});
    Transaction(cart, 0xD8, {0, 0, 0});
    assert(cart.Confirmed() && cart.Flash() && cart.AddressBytes() == 3);
    cart.FlushSRAMFile();
    NDSCart_SRAMManager::FlushSecondaryBuffer();
    assert(ReadFile(path) == original);
    assert(NDSCart::TakeSaveDetectionRestart());
    cart.LoadSave(path.c_str(), 2);
    cart.Reset();
    assert(cart.Confirmed() && cart.Flash() && cart.AddressBytes() == 3);
    assert(cart.Bytes() == original);
    WriteByte(cart, 70000, 3, 0x79, true);
    assert(ReadByte(cart, 70000, 3) == 0x79);
    cart.FlushSRAMFile();
    NDSCart_SRAMManager::FlushSecondaryBuffer();
    auto saved = ReadFile(path);
    assert(saved.size() == 131072 && saved[70000] == 0x79);
}

void CheckUnknownImportPreservesBytes()
{
    Prepare(true);
    const auto path = Path("unknown-import");
    WriteFile(path, std::vector<u8>(512, 0xFF));
    TestCart cart;
    cart.LoadSave(path.c_str(), 2);
    cart.Reset();
    ConfirmAddress(cart, 2);
    WriteByte(cart, 4, 2, 0x11, false);
    cart.FlushSRAMFile();
    assert(NDSCart_SRAMManager::NeedsFlush());
    std::vector<u8> imported(512 * 1024, 0xFF);
    imported[65535] = 0x6D;
    assert(cart.ImportSRAM(imported.data(), imported.size()) == 0);
    assert(cart.Bytes() == imported && !cart.Confirmed());
    assert(!NDSCart_SRAMManager::NeedsFlush());
    NDSCart_SRAMManager::FlushSecondaryBuffer();
    assert(ReadFile(path) == imported);
    ConfirmAddress(cart, 2);
    assert(!NDSCart::TakeSaveDetectionRestart());
    WriteByte(cart, 19, 2, 0x6E, false);
    cart.FlushSRAMFile();
    NDSCart_SRAMManager::FlushSecondaryBuffer();
    imported[19] = 0x6E;
    assert(ReadFile(path) == imported);
}

void CheckChangedSavestateRestoresRestart()
{
    Prepare(true);
    const auto path = Path("unknown-state-restart");
    const std::vector<u8> original(512, 0xFF);
    WriteFile(path, original);
    TestCart cart;
    cart.LoadSave(path.c_str(), 2);
    cart.Reset();
    ConfirmAddress(cart, 1);
    assert(cart.Changed());
    Savestate changed(true);
    cart.DoSavestate(&changed);

    Prepare(true);
    cart.LoadSave(path.c_str(), 2);
    cart.Reset();
    assert(!cart.Confirmed() && !cart.Changed());
    assert(!NDSCart::TakeSaveDetectionRestart());
    Savestate restored(changed.Data);
    cart.DoSavestate(&restored);
    assert(cart.Confirmed() && cart.Changed() && cart.AddressBytes() == 1);
    cart.FlushSRAMFile();
    assert(!NDSCart_SRAMManager::NeedsFlush());
    NDSCart_SRAMManager::FlushSecondaryBuffer();
    assert(ReadFile(path) == original);
    assert(NDSCart::TakeSaveDetectionRestart());
    assert(!NDSCart::TakeSaveDetectionRestart());
    cart.LoadSave(path.c_str(), 2);
    cart.Reset();
    assert(cart.Confirmed() && !cart.Changed() && cart.AddressBytes() == 1);
    assert(cart.Bytes() == original);
}

void CheckLegacySparseSavestateUsesStoredLength()
{
    Prepare(true);
    const auto path = Path("unknown-legacy-state");
    std::vector<u8> original(8192, 0xFF);
    original[0] = 0x37;
    WriteFile(path, original);
    TestCart cart;
    cart.LoadSave(path.c_str(), 2);
    cart.Reset();
    assert(cart.AddressBytes() == 1 && !cart.Confirmed());
    Savestate legacy(true);
    legacy.VersionMinor = 0;
    cart.DoSavestate(&legacy);

    WriteFile(path, std::vector<u8>(512, 0xFF));
    cart.LoadSave(path.c_str(), 2);
    cart.Reset();
    Savestate restored(legacy.Data);
    restored.VersionMinor = 0;
    cart.DoSavestate(&restored);
    assert(cart.Length() == 8192 && cart.Bytes() == original);
    assert(cart.Confirmed() && !cart.Changed() && cart.AddressBytes() == 2);
    assert(!NDSCart::TakeSaveDetectionRestart());
    assert(ReadByte(cart, 0, 2) == original[0]);
    NDSCart_SRAMManager::FlushSecondaryBuffer();
    assert(ReadFile(path) == original);
}

void CheckUnknownImportAliasesCurrentBuffer()
{
    Prepare(true);
    const auto path = Path("unknown-import-alias");
    std::vector<u8> original(8192, 0xFF);
    original[4000] = 0x49;
    WriteFile(path, original);
    TestCart cart;
    cart.LoadSave(path.c_str(), 2);
    cart.Reset();
    ConfirmAddress(cart, 2);
    WriteByte(cart, 20, 2, 0x4A, false);
    cart.FlushSRAMFile();
    assert(NDSCart_SRAMManager::NeedsFlush());
    const auto expected = cart.Bytes();
    assert(cart.ImportSRAM(cart.Data(), cart.Length()) == 0);
    assert(cart.Bytes() == expected && !cart.Confirmed());
    assert(!NDSCart_SRAMManager::NeedsFlush());
    NDSCart_SRAMManager::FlushSecondaryBuffer();
    assert(ReadFile(path) == expected);
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    TestDirectory = argv[1];
    assert(NDSCart_SRAMManager::Init());
    CheckKnownSaveTypes();
    CheckKnownSavestateResize();
    CheckKnownEmptySaveReload();
    CheckUnknownPaddedInput();
    CheckUnknownAddressWidths();
    CheckHeldChipSelect();
    CheckPendingSnapshotSurvivesGrowth();
    CheckUpdateBufferAfterGrowth();
    CheckAmbiguousTrafficDoesNotPersist();
    CheckUnknownSavestateDiscardsPendingFlush();
    CheckUnknownFlashRestart();
    CheckUnknownImportPreservesBytes();
    CheckChangedSavestateRestoresRestart();
    CheckLegacySparseSavestateUsesStoredLength();
    CheckUnknownImportAliasesCurrentBuffer();
    NDSCart_SRAMManager::DeInit();
    std::cout << "Real cartridge/save-manager integration passed\n";
}
