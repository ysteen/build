#include <cassert>
#include <iostream>
#include <map>
#include <set>
#include "NDSCart_SaveDetection.h"

using namespace NDSCart;

static u32 code(const char* value)
{
    return u32(value[0]) | u32(value[1]) << 8 | u32(value[2]) << 16 | u32(value[3]) << 24;
}

int main()
{
    const u32 patchedSize = 256 * 1024 * 1024;
    const auto layton = DetectSaveMemory(code("C3JK"), patchedSize, false, -1);
    assert(layton.Source == SaveDetectionSource::Regional);
    assert(layton.Params.SaveMemType == 3 && SaveMemorySize(layton.Params.SaveMemType) == 65536);
    assert(layton.Params.ROMSize == patchedSize);

    const auto mario = DetectSaveMemory(code("CLJK"), 128 * 1024 * 1024, false, -1);
    assert(mario.Source == SaveDetectionSource::Database);
    assert(mario.Params.SaveMemType == 2 && SaveMemorySize(mario.Params.SaveMemType) == 8192);
    assert(DetectSaveMemory(code("ASMK"), patchedSize, false, -1).Params.SaveMemType == 1);
    assert(DetectSaveMemory(code("ASME"), patchedSize, false, -1).Params.SaveMemType == 2);

    std::map<u32, std::set<u32>> families;
    std::set<u32> knownCodes;
    u32 previous = 0;
    unsigned korean = 0;
    for (const auto& entry : ROMList)
    {
        assert(entry.GameCode > previous);
        previous = entry.GameCode;
        const auto detected = DetectSaveMemory(entry.GameCode, patchedSize, false, -1);
        assert(detected.Source == SaveDetectionSource::Database);
        assert(detected.Params.GameCode == entry.GameCode);
        assert(detected.Params.ROMSize == entry.ROMSize);
        assert(detected.Params.SaveMemType == entry.SaveMemType);
        families[entry.GameCode & 0xFFFFFF].insert(entry.SaveMemType);
        knownCodes.insert(entry.GameCode);
        korean += (entry.GameCode >> 24) == 'K';
    }

    unsigned inferred = 0, ambiguous = 0;
    std::set<u32> coveredTypes;
    for (const auto& family : families)
    {
        u32 missingCode = 0;
        for (u32 region = 'A'; region <= 'Z'; region++)
        {
            const u32 candidate = family.first | region << 24;
            if (!knownCodes.count(candidate)) { missingCode = candidate; break; }
        }
        assert(missingCode != 0);
        const auto detected = DetectSaveMemory(missingCode, patchedSize, false, -1);
        if (family.second.size() == 1 && *family.second.begin() <= 10)
        {
            assert(detected.Source == SaveDetectionSource::Regional);
            assert(detected.Params.SaveMemType == *family.second.begin());
            coveredTypes.insert(detected.Params.SaveMemType);
            inferred++;
        }
        else
        {
            assert(detected.Source == SaveDetectionSource::Fallback);
            ambiguous++;
        }
    }
    for (u32 type = 1; type <= 10; type++) assert(coveredTypes.count(type));

    for (const char* game : {"ZZZK", "ASMQ", "A36Q", "BJAQ", "C3J#", "####"})
    {
        const auto result = DetectSaveMemory(code(game), patchedSize, false, -1);
        assert(result.Source == SaveDetectionSource::Fallback);
        assert(result.Params.SaveMemType == 2);
    }
    assert(DetectSaveMemory(code("C3JK"), patchedSize, true, 6).Source == SaveDetectionSource::Homebrew);
    assert(DetectSaveMemory(code("C3JK"), patchedSize, true, 6).Params.SaveMemType == 0);

    const u32 sizes[] = {0, 512, 8192, 65536, 131072, 262144, 524288, 1048576, 8388608, 16777216, 67108864};
    for (u32 type = 0; type <= 10; type++)
    {
        assert(SaveMemorySize(type) == sizes[type]);
        assert(ParseSaveMemoryOverride(SaveMemoryTypeName(type)) == static_cast<int>(type));
        for (const char* game : {"C3JK", "CLJK", "ASMQ", "ZZZK"})
        {
            const auto result = DetectSaveMemory(code(game), patchedSize, false, type);
            assert(result.Source == SaveDetectionSource::Override);
            assert(result.Params.SaveMemType == type);
        }
    }
    assert(ParseSaveMemoryOverride(nullptr) == -1);
    for (const char* invalid : {"", "Auto", "512", "64 KiB", "Flash 2 MiB"})
        assert(ParseSaveMemoryOverride(invalid) == -1);
    assert(SaveMemorySize(11) == 0 && SaveMemorySize(0xFFFFFFFF) == 0);
    assert(DetectSaveMemory(code("C3JK"), patchedSize, false, 11).Source == SaveDetectionSource::Regional);
    assert(DetectSaveMemory(code("CLJK"), patchedSize, false, -2).Source == SaveDetectionSource::Database);

    std::cout << knownCodes.size() << " exact entries (" << korean << " Korean), "
              << inferred << " regional families, " << ambiguous << " ambiguous/unknown families, "
              << "all 11 save types and overrides passed\n";
}
