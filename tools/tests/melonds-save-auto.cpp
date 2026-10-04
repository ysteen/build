#include <cassert>
#include <iostream>
#include <vector>
#include "NDSCart_SaveAuto.h"

using namespace NDSCart;

static void checkHints()
{
    SaveAutoDetector detector;
    detector.Initialize(nullptr, 0);
    assert(detector.AddressBytes == 2 && !detector.Confirmed && !detector.Changed);
    detector.Initialize(nullptr, 524288);
    assert(detector.AddressBytes == 2);

    std::vector<u8> save(524288, 0xFF);
    detector.Initialize(save.data(), save.size());
    assert(detector.AddressBytes == 2 && !detector.Flash);

    save[511] = 0;
    detector.Initialize(save.data(), save.size());
    assert(detector.AddressBytes == 1 && !detector.Confirmed);
    save[512] = 0;
    detector.Initialize(save.data(), save.size());
    assert(detector.AddressBytes == 2);
    save[8191] = 0;
    detector.Initialize(save.data(), 8192);
    assert(detector.AddressBytes == 2 && !detector.Confirmed);

    save[65535] = 0;
    const auto original = save;
    detector.Initialize(save.data(), save.size());
    assert(detector.AddressBytes == 2 && !detector.Flash);
    assert(save == original);

    save[65536] = 0;
    detector.Initialize(save.data(), save.size());
    assert(detector.AddressBytes == 3 && !detector.Flash && !detector.Confirmed);
    save[131071] = 0;
    detector.Initialize(save.data(), 131072);
    assert(detector.AddressBytes == 3 && !detector.Flash);
}

static void checkReadTraces()
{
    for (u32 width = 1; width <= 3; width++)
    {
        SaveAutoDetector detector;
        detector.Initialize(nullptr, 0);
        detector.Observe(0x9F, 3);
        detector.Observe(0x05, 1);
        detector.Observe(0x03, width + 1);
        assert(!detector.Confirmed && detector.AddressBytes == 2);
        assert(detector.CandidateBytes == width && detector.CandidateVotes == 1);
        detector.Observe(0x03, width + 256);
        assert(detector.Confirmed && detector.AddressBytes == width);
        assert(detector.Changed == (width != 2));
        assert(!detector.Flash && detector.Transactions == 4);

        detector.Observe(0x03, width == 1 ? 4 : 2);
        assert(detector.AddressBytes == width && detector.CandidateVotes == 2);
    }

    SaveAutoDetector detector;
    detector.Initialize(nullptr, 0);
    detector.Observe(0x03, 3);
    detector.Observe(0x03, 4);
    assert(!detector.Confirmed && detector.CandidateBytes == 3 && detector.CandidateVotes == 1);
    detector.Observe(0x03, 7);
    assert(detector.Confirmed && detector.AddressBytes == 3 && detector.Changed);
}

static void checkInsufficientEvidence()
{
    SaveAutoDetector detector;
    detector.Initialize(nullptr, 0);
    for (const u32 length : {0u, 1u, 8u, 12u, 256u}) detector.Observe(0x03, length);
    for (const u8 command : {0x02, 0x0A, 0x0B, 0x05, 0x9F})
    {
        detector.Observe(command, 3);
        detector.Observe(command, 259);
    }
    assert(!detector.Confirmed && !detector.Flash && !detector.Changed);
    assert(detector.CandidateVotes == 0 && detector.AddressBytes == 2);
    detector.Observe(0x03, 4);
    detector.Observe(0x02, 258);
    assert(!detector.Confirmed && detector.CandidateVotes == 1);
    assert(detector.CandidateBytes == 3);
    detector.Observe(0x02, 259);
    assert(detector.Confirmed && detector.AddressBytes == 3);

    detector.Initialize(nullptr, 0);
    detector.Observe(0x0A, 257);
    assert(detector.CandidateVotes == 0);
    detector.Observe(0x03, 2);
    detector.Observe(0x0A, 257);
    assert(detector.Confirmed && detector.AddressBytes == 1 && !detector.Flash);
}

static void checkFlashEvidence()
{
    for (const u8 command : {0xD8, 0xDB})
    {
        SaveAutoDetector detector;
        detector.Initialize(nullptr, 0);
        detector.Observe(command, 2);
        detector.Observe(command, 4);
        assert(!detector.Confirmed && !detector.Flash);
        detector.Observe(command, 3);
        assert(detector.AddressBytes == 3 && detector.Confirmed && detector.Flash && detector.Changed);
        detector.Observe(0x03, 3);
        assert(detector.AddressBytes == 3 && detector.Flash);

        detector.Initialize(nullptr, 0, 3);
        detector.Observe(command, 3);
        assert(detector.Confirmed && detector.Flash && !detector.Changed);
        detector.Initialize(nullptr, 0, 1);
        detector.Observe(command, 3);
        assert(detector.AddressBytes == 1 && !detector.Flash && !detector.Changed);
    }
}

static void checkRestartAndReset()
{
    SaveAutoDetector detector;
    detector.Initialize(nullptr, 0);
    detector.Observe(0x03, 4);
    detector.Observe(0x03, 7);
    assert(detector.Changed && detector.Confirmed);
    const u32 learned = detector.AddressBytes;
    detector.Initialize(nullptr, 0, learned, detector.Flash);
    assert(detector.Confirmed && !detector.Changed && detector.Transactions == 0);
    detector.Observe(0x03, 3);
    detector.Observe(0x03, 3);
    assert(detector.AddressBytes == 3 && !detector.Changed);
    detector.Initialize(nullptr, 0, 3, true);
    assert(detector.Confirmed && detector.Flash && !detector.Changed);
    detector.Initialize(nullptr, 0, 2, true);
    assert(detector.Confirmed && !detector.Flash);
    detector.Initialize(nullptr, 0, 4, true);
    assert(!detector.Confirmed && !detector.Flash && detector.AddressBytes == 2);
    detector.Initialize(nullptr, 0);
    assert(!detector.Confirmed && !detector.Flash && !detector.Changed);
    assert(detector.CandidateBytes == 0 && detector.CandidateVotes == 0);
    detector.Transactions = 0xFFFFFFFEu;
    detector.Observe(0x05, 1);
    detector.Observe(0x05, 1);
    assert(detector.Transactions == 0xFFFFFFFFu);
}

int main()
{
    checkHints();
    checkReadTraces();
    checkInsufficientEvidence();
    checkFlashEvidence();
    checkRestartAndReset();
    std::cout << "Save auto-detection hints, SPI traces, confidence, flash evidence, and restart tests passed\n";
}
