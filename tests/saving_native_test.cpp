// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/saving_native.h"
#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>

namespace {
using namespace g3t::saving::native;
inline constexpr char WorldLoadBody[] =
    "81ecbc000000538b5d0856578bf98d87800000005053";
void Put(std::vector<uint8_t>& image, size_t at, uint32_t value)
{
    assert(Range(image.size(), at, 4));
    std::memcpy(image.data() + at, &value, 4);
}
uint16_t Short(std::vector<uint8_t> const& image, size_t at)
{
    if (!Range(image.size(), at, 2)) throw std::runtime_error("truncated PE word");
    uint16_t value;
    std::memcpy(&value, image.data() + at, 2);
    return value;
}
uint32_t Get(std::vector<uint8_t> const& image, size_t at)
{
    if (!Range(image.size(), at, 4)) throw std::runtime_error("truncated PE dword");
    return Word(image.data(), at);
}

std::string Sha256(std::vector<uint8_t> const& input)
{
    static constexpr uint32_t constants[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    auto rotate = [](uint32_t value, unsigned count) { return (value >> count) | (value << (32 - count)); };
    uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<uint8_t> padded = input;
    const uint64_t bits = static_cast<uint64_t>(input.size()) * 8;
    padded.push_back(0x80);
    while (padded.size() % 64 != 56) padded.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8) padded.push_back(static_cast<uint8_t>(bits >> shift));
    for (size_t block = 0; block < padded.size(); block += 64) {
        uint32_t words[64]{};
        for (size_t i = 0; i < 16; ++i) {
            for (size_t b = 0; b < 4; ++b) words[i] = (words[i] << 8) | padded[block + i * 4 + b];
        }
        for (size_t i = 16; i < 64; ++i) {
            const uint32_t a = words[i - 15], b = words[i - 2];
            words[i] = words[i - 16] + (rotate(a, 7) ^ rotate(a, 18) ^ (a >> 3)) +
                       words[i - 7] + (rotate(b, 17) ^ rotate(b, 19) ^ (b >> 10));
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3],
                 e = state[4], f = state[5], g = state[6], h = state[7];
        for (size_t i = 0; i < 64; ++i) {
            const uint32_t t1 = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) +
                                ((e & f) ^ (~e & g)) + constants[i] + words[i];
            const uint32_t t2 = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) +
                                ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (uint32_t word : state)
        for (int shift = 28; shift >= 0; shift -= 4) result += hex[(word >> shift) & 15];
    return result;
}

class AddressReader {
    std::map<uint32_t, uint8_t> memory;
public:
    bool Put(uint32_t address, std::vector<uint8_t> const& bytes)
    {
        for (size_t i = 0; i < bytes.size(); ++i) {
            const uint64_t at = static_cast<uint64_t>(address) + i;
            if (at > UINT32_MAX) return false;
            memory[static_cast<uint32_t>(at)] = bytes[i];
        }
        return true;
    }
    bool PutWord(uint32_t address, uint32_t value)
    {
        std::vector<uint8_t> bytes(4);
        std::memcpy(bytes.data(), &value, 4);
        return Put(address, bytes);
    }
    void Remove(uint32_t address, size_t size)
    {
        for (size_t i = 0; i < size; ++i)
            memory.erase(address + static_cast<uint32_t>(i));
    }
    bool Read(uint32_t address, size_t size, std::vector<uint8_t>& bytes) const
    {
        bytes.clear();
        bytes.reserve(size);
        for (size_t i = 0; i < size; ++i) {
            const uint64_t at = static_cast<uint64_t>(address) + i;
            if (at > UINT32_MAX) return false;
            const auto found = memory.find(static_cast<uint32_t>(at));
            if (found == memory.end()) {
                bytes.clear();
                return false;
            }
            bytes.push_back(found->second);
        }
        return true;
    }
};

struct PairedSaveFixture {
    char const* label;
    uint32_t gameBase;
    uint32_t entryVa;
    std::vector<uint8_t> entry;
    uint32_t shimAddress;
    uint32_t pupBase;
    bool syntheticPup;
};

std::vector<uint8_t> SyntheticReceiver(uint32_t pupBase)
{
    const auto receiver = Decode(PupReceiverBytes);
    assert(receiver.size() == 41);
    std::vector<uint8_t> callback(41);
    for (size_t i = 0; i < receiver.size(); ++i)
        callback[i] = receiver[i] < 0 ? 0 : static_cast<uint8_t>(receiver[i]);
    Put(callback, 7, pupBase + PupKnownSemaphore);
    Put(callback, 13, pupBase + PupKnownWaitImport);
    Put(callback, 23, pupBase + PupKnownSemaphore);
    Put(callback, 29, pupBase + PupKnownReleaseImport);
    Put(callback, 34, pupBase + PupKnownAdapter);
    PupLayout layout{};
    assert(PupReceiverLayoutAt(callback.data(), callback.size(), pupBase + PupKnownRva,
                               pupBase, PupKnownImageSize, layout));
    assert(PupLayoutIsPinned(PupKnownRva, PupKnownImageSize, layout));
    return callback;
}

bool KnownReceiver(uint8_t const* data, size_t size, uint32_t base)
{
    PupLayout layout{};
    return PupReceiverLayoutAt(data, size, base + PupKnownRva, base, PupKnownImageSize, layout) &&
           PupLayoutIsPinned(PupKnownRva, PupKnownImageSize, layout);
}

void StructuralReceiverFixtures()
{
    const uint32_t base = 0x41000000u, imageSize = 0x50000u, rva = 0x1200u;
    auto callback = SyntheticReceiver(base);
    Put(callback, 7, base + 0x31000);
    Put(callback, 13, base + 0x32000);
    Put(callback, 23, base + 0x31000);
    Put(callback, 29, base + 0x33000);
    Put(callback, 34, base + 0x34000);
    PupLayout layout{};
    const auto accepted = [&](std::vector<uint8_t> const& bytes) {
        return PupReceiverLayoutAt(bytes.data(), bytes.size(), base + rva, base, imageSize, layout);
    };
    assert(accepted(callback));
    assert(layout.semaphore == 0x31000 && layout.waitImport == 0x32000 &&
           layout.releaseImport == 0x33000 && layout.adapter == 0x34000);
    assert(!PupLayoutIsPinned(rva, imageSize, layout));
    assert(!PupReceiverLayoutAt(callback.data(), callback.size(), base - 1, base, imageSize, layout));
    assert(!PupReceiverLayoutAt(callback.data(), callback.size(), base + imageSize, base, imageSize, layout));

    for (size_t operand : {7u, 13u, 29u, 34u}) {
        for (uint32_t bad : {base - 4, base + imageSize, Get(callback, operand) + 1}) {
            auto corrupt = callback;
            Put(corrupt, operand, bad);
            if (operand == 7) Put(corrupt, 23, bad);
            assert(!accepted(corrupt));
        }
    }
    auto corrupt = callback;
    Put(corrupt, 23, base + 0x31004);
    assert(!accepted(corrupt));
    corrupt = callback;
    Put(corrupt, 29, Get(corrupt, 13));
    assert(!accepted(corrupt));
    for (size_t operand : {7u, 13u, 29u}) {
        corrupt = callback;
        Put(corrupt, 34, Get(corrupt, operand));
        assert(!accepted(corrupt));
    }
    corrupt = callback;
    corrupt[12] ^= 1;
    assert(!accepted(corrupt));
    assert(!PupReceiverLayoutAt(callback.data(), 40, base + rva, base, imageSize, layout));

    const auto known = SyntheticReceiver(base);
    assert(PupReceiverLayoutAt(known.data(), known.size(), base + PupKnownRva,
                               base, PupKnownImageSize, layout));
    assert(PupLayoutIsPinned(PupKnownRva, PupKnownImageSize, layout));
    assert(!PupLayoutIsPinned(PupKnownRva + 4, PupKnownImageSize, layout));
    assert(!PupLayoutIsPinned(PupKnownRva, PupKnownImageSize + 4, layout));
    for (unsigned field = 0; field < 4; ++field) {
        auto different = layout;
        switch (field) {
        case 0: different.semaphore += 4; break;
        case 1: different.waitImport += 4; break;
        case 2: different.releaseImport += 4; break;
        default: different.adapter += 4; break;
        }
        assert(!PupLayoutIsPinned(PupKnownRva, PupKnownImageSize, different));
    }
}

void CheckPairedSaveFixture(PairedSaveFixture const& fixture)
{
    const uint32_t entryRva = fixture.entryVa - fixture.gameBase;
    assert(entryRva == 0x19d170u);

    const auto worldBody = Decode(WorldLoadBody);
    assert(worldBody.size() >= 6);
    assert(fixture.entry.size() == 12);
    assert(std::equal(worldBody.begin(), worldBody.begin() + 6,
                      fixture.entry.begin() + 6));
    const auto diskEntry = Decode(Sites[3].bytes);
    assert(diskEntry.size() > 5);
    assert(diskEntry[0] == 0x55 && diskEntry[1] == 0x8b &&
           diskEntry[2] == 0xec && diskEntry[3] == 0x83 &&
           diskEntry[4] == 0xe4 && diskEntry[5] == 0xf8);

    AddressReader runtime;
    assert(runtime.Put(fixture.entryVa, fixture.entry));

    std::vector<uint8_t> entryBytes;
    assert(runtime.Read(fixture.entryVa, fixture.entry.size(), entryBytes));
    uint32_t shimAddress = 0;
    assert(PupEntryTarget(entryBytes.data(), entryBytes.size(), fixture.entryVa, shimAddress));
    assert(shimAddress == fixture.shimAddress);

    std::vector<uint8_t> shim(9);
    shim = {0x87, 0x0c, 0x24, 0x51, 0xe9, 0, 0, 0, 0};
    const uint32_t callbackAddress = fixture.pupBase + PupKnownRva;
    const uint32_t shimNext = fixture.shimAddress + 9;
    Put(shim, 5, callbackAddress - shimNext);
    if (!fixture.syntheticPup) {
        // Earlier captured shim: 87 0c 24 51 e9 27 16 ae 70.
        shim = {0x87, 0x0c, 0x24, 0x51, 0xe9, 0x27, 0x16, 0xae, 0x70};
        assert(callbackAddress == 0x724c57f0u);
    }
    assert(runtime.Put(fixture.shimAddress, shim));

    std::vector<uint8_t> shimBytes;
    assert(runtime.Read(fixture.shimAddress, shim.size(), shimBytes));
    uint32_t decodedCallback = 0;
    assert(PupShimTarget(shimBytes.data(), shimBytes.size(), fixture.shimAddress, decodedCallback));
    assert(decodedCallback == callbackAddress);

    const std::vector<uint8_t> receiver = SyntheticReceiver(fixture.pupBase);
    assert(runtime.Put(callbackAddress, receiver));
    std::vector<uint8_t> callbackBytes;
    assert(runtime.Read(callbackAddress, receiver.size(), callbackBytes));
    PupLayout layout{};
    assert(PupReceiverLayoutAt(callbackBytes.data(), callbackBytes.size(),
                               callbackAddress, fixture.pupBase, PupKnownImageSize, layout));
    assert(PupLayoutIsPinned(PupKnownRva, PupKnownImageSize, layout));
    assert(!PupReceiverLayoutAt(callbackBytes.data(), callbackBytes.size(),
                                fixture.pupBase - 1, fixture.pupBase, PupKnownImageSize, layout));
    assert(!PupReceiverLayoutAt(callbackBytes.data(), callbackBytes.size(),
                                callbackAddress, fixture.pupBase + PupKnownRva + 4, PupKnownImageSize, layout));
    assert(!PupReceiverLayoutAt(callbackBytes.data(), callbackBytes.size(),
                                callbackAddress, fixture.pupBase, PupKnownRva, layout));

    auto foreign = receiver;
    assert(!PupReceiverLayoutAt(foreign.data(), foreign.size(), callbackAddress,
                                fixture.pupBase + PupKnownImageSize, PupKnownImageSize, layout));

    auto truncatedEntry = fixture.entry;
    AddressReader truncatedEntryReader;
    truncatedEntry.pop_back();
    assert(truncatedEntryReader.Put(fixture.entryVa, truncatedEntry));
    assert(!truncatedEntryReader.Read(fixture.entryVa, 12, entryBytes));
    assert(runtime.Read(fixture.entryVa, 12, entryBytes));
    assert(!PupEntryTarget(truncatedEntry.data(), 5, fixture.entryVa, shimAddress));

    auto changedEntryOpcode = fixture.entry;
    changedEntryOpcode[0] = 0xe8;
    assert(!PupEntryTarget(changedEntryOpcode.data(), changedEntryOpcode.size(),
                           fixture.entryVa, shimAddress));
    auto changedEntryNop = fixture.entry;
    changedEntryNop[5] = 0x91;
    assert(!PupEntryTarget(changedEntryNop.data(), changedEntryNop.size(),
                           fixture.entryVa, shimAddress));
    auto changedEntryTail = fixture.entry;
    changedEntryTail[6] = 0x82;
    assert(!std::equal(worldBody.begin(), worldBody.begin() + 6,
                       changedEntryTail.begin() + 6));
    AddressReader changedTailReader;
    assert(changedTailReader.Put(fixture.entryVa, changedEntryTail));
    assert(changedTailReader.Read(fixture.entryVa, 12, entryBytes));
    assert(!std::equal(worldBody.begin(), worldBody.begin() + 6,
                       entryBytes.begin() + 6));

    auto mutatedEntry = fixture.entry;
    const uint32_t negativeRel32 = 0xffffff00u;
    std::memcpy(mutatedEntry.data() + 1, &negativeRel32, 4);
    assert(runtime.Put(fixture.entryVa, mutatedEntry));
    assert(runtime.Read(fixture.entryVa, mutatedEntry.size(), entryBytes));
    uint32_t mutatedShimAddress = 0;
    assert(PupEntryTarget(entryBytes.data(), entryBytes.size(),
                          fixture.entryVa, mutatedShimAddress));
    assert(mutatedShimAddress == fixture.entryVa + 5u + negativeRel32);
    assert(mutatedShimAddress != fixture.shimAddress);
    std::vector<uint8_t> staleShim;
    assert(!runtime.Read(mutatedShimAddress, 9, staleShim));

    auto changedShim = shim;
    for (size_t i = 0; i < 4; ++i) {
        changedShim = shim;
        changedShim[i] ^= 1;
        assert(!PupShimTarget(changedShim.data(), changedShim.size(),
                              fixture.shimAddress, decodedCallback));
    }
    changedShim = shim;
    changedShim[4] = 0xea;
    assert(!PupShimTarget(changedShim.data(), changedShim.size(),
                          fixture.shimAddress, decodedCallback));
    assert(!PupShimTarget(shim.data(), 8, fixture.shimAddress, decodedCallback));
    AddressReader truncatedShimReader;
    auto truncatedShim = shim;
    truncatedShim.pop_back();
    assert(truncatedShimReader.Put(fixture.shimAddress, truncatedShim));
    assert(!truncatedShimReader.Read(fixture.shimAddress, 9, shimBytes));

    changedShim = shim;
    uint32_t negativeShimRel32 = 0xfffffe00u;
    std::memcpy(changedShim.data() + 5, &negativeShimRel32, 4);
    assert(runtime.Put(fixture.shimAddress, changedShim));
    assert(runtime.Read(fixture.shimAddress, 9, shimBytes));
    uint32_t negativeCallbackAddress = 0;
    assert(PupShimTarget(shimBytes.data(), shimBytes.size(),
                         fixture.shimAddress, negativeCallbackAddress));
    assert(negativeCallbackAddress == fixture.shimAddress + 9u + negativeShimRel32);
    assert(negativeCallbackAddress != callbackAddress);
    std::vector<uint8_t> staleCallback;
    assert(!runtime.Read(negativeCallbackAddress, 41, staleCallback));

    auto truncatedReceiver = callbackBytes;
    assert(!KnownReceiver(truncatedReceiver.data(), 40, fixture.pupBase));
    AddressReader truncatedReceiverReader;
    truncatedReceiver.pop_back();
    assert(truncatedReceiverReader.Put(callbackAddress, truncatedReceiver));
    assert(!truncatedReceiverReader.Read(callbackAddress, 41, callbackBytes));
    callbackBytes = receiver;
    for (size_t at : {0u, 6u, 12u, 22u, 28u, 33u, 40u}) {
        auto corruptFixed = callbackBytes;
        corruptFixed[at] ^= 1;
        assert(!KnownReceiver(corruptFixed.data(), corruptFixed.size(), fixture.pupBase));
    }
    for (size_t operand : {7u, 13u, 23u, 29u, 34u}) {
        auto corruptOperand = callbackBytes;
        Put(corruptOperand, operand, Get(corruptOperand, operand) + 1);
        assert(!KnownReceiver(corruptOperand.data(), corruptOperand.size(), fixture.pupBase));
    }
}

void PairedSaveFixtures()
{
    const std::vector<PairedSaveFixture> fixtures{
        {"fixture1 synthetic PUP owner", 0x01f90000u, 0x0212d170u,
         {0xe9, 0x4b, 0x70, 0x65, 0xff, 0x90, 0x81, 0xec, 0xbc, 0x00, 0x00, 0x00},
         0x017841c0u, 0x71000000u, true},
        {"fixture2 synthetic PUP owner", 0x10000000u, 0x1019d170u,
         {0xe9, 0x4b, 0x70, 0x9e, 0xf1, 0x90, 0x81, 0xec, 0xbc, 0x00, 0x00, 0x00},
         0x01b841c0u, 0x73000000u, true},
        {"fixture3 earlier actual PUP owner", 0x00ea0000u, 0x0103d170u,
         {0xe9, 0x4b, 0x70, 0x9a, 0x00, 0x90, 0x81, 0xec, 0xbc, 0x00, 0x00, 0x00},
         0x019e41c0u, 0x724b0000u, false},
    };
    for (auto const& fixture : fixtures) {
        CheckPairedSaveFixture(fixture);
    }
}
void Fixture()
{
    static_assert(Stolen == 6);
    assert(Sha256({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string abc = "abc";
    assert(Sha256({abc.begin(), abc.end()}) ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string longMessage = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    assert(Sha256({longMessage.begin(), longMessage.end()}) ==
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const std::vector<int> prologue{0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};
    for (size_t i = 0; i < 4; ++i) {
        const auto bytes = Decode(Sites[i].bytes);
        assert(bytes.size() >= Stolen && std::equal(prologue.begin(), prologue.end(), bytes.begin()));
    }

    uint32_t target = 0;
    std::vector<uint8_t> entry{0xe9, 0x4b, 0x70, 0x9a, 0x00, 0x90};
    assert(PupEntryTarget(entry.data(), entry.size(), 0x0103d170u, target));
    assert(target == 0x019e41c0u);
    assert(!PupEntryTarget(entry.data(), 5, 0x0103d170u, target));
    auto wrongOpcode = entry;
    wrongOpcode[0] = 0xe8;
    assert(!PupEntryTarget(wrongOpcode.data(), wrongOpcode.size(), 0x0103d170u, target));
    auto wrongFixed = entry;
    wrongFixed[5] = 0x00;
    assert(!PupEntryTarget(wrongFixed.data(), wrongFixed.size(), 0x0103d170u, target));
    wrongFixed = entry;
    wrongFixed[1] = 0x4c;
    assert(PupEntryTarget(wrongFixed.data(), wrongFixed.size(), 0x0103d170u, target));
    assert(target != 0x019e41c0u);

    std::vector<uint8_t> shim{0x87, 0x0c, 0x24, 0x51, 0xe9, 0x27, 0x16, 0xae, 0x70};
    assert(PupShimTarget(shim.data(), shim.size(), 0x019e41c0u, target));
    assert(target == 0x724c57f0u);
    assert(!PupShimTarget(shim.data(), 8, 0x019e41c0u, target));
    auto wrongShim = shim;
    for (size_t i = 0; i < 4; ++i) {
        wrongShim = shim;
        wrongShim[i] ^= 1;
        assert(!PupShimTarget(wrongShim.data(), wrongShim.size(), 0x019e41c0u, target));
    }
    wrongShim = shim;
    wrongShim[4] = 0xea;
    assert(!PupShimTarget(wrongShim.data(), wrongShim.size(), 0x019e41c0u, target));
    wrongShim = shim;
    wrongShim[5] = 0x28;
    assert(PupShimTarget(wrongShim.data(), wrongShim.size(), 0x019e41c0u, target));
    assert(target != 0x724c57f0u);

    std::vector<uint8_t> branch{0xe9, 0x20, 0x00, 0x00, 0x00};
    assert(BranchTarget(branch.data(), branch.size(), 0x100u, 0xe9, target) && target == 0x125u);
    branch = {0xe8, 0xfe, 0xff, 0xff, 0xff};
    assert(BranchTarget(branch.data(), branch.size(), 0x10000020u, 0xe8, target));
    assert(target == 0x10000023u);
    branch = {0xe9, 0xe0, 0xff, 0xff, 0xff};
    assert(BranchTarget(branch.data(), branch.size(), 0x10000020u, 0xe9, target));
    assert(target == 0x10000005u);
    branch = {0xe9, 0x20, 0x00, 0x00, 0x00};
    assert(BranchTarget(branch.data(), branch.size(), 0xfffffff0u, 0xe9, target));
    assert(target == 0x00000015u);
    assert(!BranchTarget(branch.data(), 4, 0x100u, 0xe9, target));
    assert(!BranchTarget(branch.data(), branch.size(), 0x100u, 0xe8, target));

    const uint32_t pupBase = 0x724b0000u;
    std::vector<uint8_t> callback(PupKnownImageSize, 0xcc);
    const size_t callbackRva = PupKnownRva;
    const auto receiver = Decode(PupReceiverBytes);
    assert(receiver.size() == 41);
    for (size_t i = 0; i < receiver.size(); ++i)
        callback[callbackRva + i] = receiver[i] < 0 ? 0 : static_cast<uint8_t>(receiver[i]);
    Put(callback, callbackRva + 7, pupBase + PupKnownSemaphore);
    Put(callback, callbackRva + 13, pupBase + PupKnownWaitImport);
    Put(callback, callbackRva + 23, pupBase + PupKnownSemaphore);
    Put(callback, callbackRva + 29, pupBase + PupKnownReleaseImport);
    Put(callback, callbackRva + 34, pupBase + PupKnownAdapter);
    assert(KnownReceiver(callback.data() + callbackRva, callback.size() - callbackRva, pupBase));
    assert(!KnownReceiver(callback.data() + callbackRva, 40, pupBase));
    for (size_t at : {callbackRva, callbackRva + 6, callbackRva + 12,
                      callbackRva + 22, callbackRva + 28, callbackRva + 33, callbackRva + 40}) {
        auto corrupt = callback;
        corrupt[at] ^= 1;
        assert(!KnownReceiver(corrupt.data() + callbackRva, corrupt.size() - callbackRva, pupBase));
    }
    for (size_t operand : {7u, 13u, 23u, 29u, 34u}) {
        auto corrupt = callback;
        Put(corrupt, callbackRva + operand, Get(corrupt, callbackRva + operand) + 1);
        assert(!KnownReceiver(corrupt.data() + callbackRva, corrupt.size() - callbackRva, pupBase));
    }
    assert(!KnownReceiver(callback.data() + callbackRva,
                          callback.size() - callbackRva, pupBase + 1));
    const uint32_t base = 0x31000000;
    std::vector<uint8_t> image(4096, 0xcc);
    size_t sites[5]{0x100, 0x300, 0x700, 0x740, 0x800};
    for (size_t i = 0; i < 5; ++i) {
        const auto pattern = Decode(Sites[i].bytes);
        for (size_t b = 0; b < pattern.size(); ++b)
            image[sites[i] + b] = pattern[b] < 0 ? 0 : static_cast<uint8_t>(pattern[b]);
        unsigned count = 0;
        assert(FindUnique(image.data(), image.size(), Sites[i].bytes, count) == sites[i] && count == 1);
    }
    unsigned bodyMatches = 0;
    assert(FindUnique(image.data(), image.size(), WorldLoadBody, bodyMatches) ==
           sites[3] + Stolen && bodyMatches == 1);
    auto modifiedEntry = image;
    std::fill_n(modifiedEntry.begin() + sites[3], Stolen, 0x90);
    assert(FindUnique(modifiedEntry.data(), modifiedEntry.size(), WorldLoadBody, bodyMatches) ==
           sites[3] + Stolen && bodyMatches == 1);
    const auto worldBody = Decode(WorldLoadBody);
    std::copy_n(modifiedEntry.begin() + sites[3] + Stolen, worldBody.size(),
                modifiedEntry.begin() + 0xd00);
    assert(FindUnique(modifiedEntry.data(), modifiedEntry.size(), WorldLoadBody, bodyMatches) ==
           Missing && bodyMatches == 2);
    assert(KnownReceiver(callback.data() + callbackRva,
                         callback.size() - callbackRva, pupBase));
    assert(pupBase + 0x157f0 != 0x720c57f0u);
    for (auto pattern : {"", "5", "zz", "?5", "55?"}) {
        unsigned count = 1;
        assert(FindUnique(image.data(), image.size(), pattern, count) == Missing && count == 0);
    }
    assert(!Range(4, 5, 0) && !Range(4, 3, 2) && Range(4, 4, 0));
    for (size_t i = 0; i < 2; ++i) {
        const size_t call = sites[i] + (i ? 0x2b2 : 0xb9);
        image[call] = 0xe8;
        Put(image, call + 1, static_cast<uint32_t>(sites[i + 2] - call - 5));
    }
    const size_t getter = sites[4];
    Put(image, getter + 7, base + 0x980);
    Put(image, getter + 15, base + 0x980);
    Put(image, getter + 28, base + 0x900);
    Put(image, getter + 32, base + 0xa00);
    Put(image, getter + 37, base + 0x904);
    Put(image, getter + 42, base + 0x908);
    Put(image, getter + 55, base + 0x900);
    Put(image, 0xa00, base + 0x100);
    size_t owner = 0, table = 0;
    assert(Links(image.data(), image.size(), base, sites, owner, table) && owner == 0x900 && table == 0xa00);
    assert(!Links(image.data(), image.size(), 0x10000000, sites, owner, table));
    auto corrupt = image;
    corrupt[sites[0] + 0xb9] = 0x90;
    assert(!Links(corrupt.data(), corrupt.size(), base, sites, owner, table));
    corrupt = image;
    Put(corrupt, getter + 42, base + 0x904);
    assert(!Links(corrupt.data(), corrupt.size(), base, sites, owner, table));
    corrupt = image;
    Put(corrupt, getter + 32, base - 4);
    assert(!Links(corrupt.data(), corrupt.size(), base, sites, owner, table));
    unsigned count = 0;
    corrupt = image;
    corrupt[sites[0]] = 0x90;
    assert(FindUnique(corrupt.data(), corrupt.size(), Sites[0].bytes, count) == Missing && count == 0);
    const auto copy = Decode(Sites[0].bytes);
    std::copy_n(image.begin() + sites[0], copy.size(), image.begin() + 0xc00);
    assert(FindUnique(image.data(), image.size(), Sites[0].bytes, count) == Missing && count == 2);
}
struct Section { uint32_t at, size, flags; };
struct PeImage {
    std::vector<uint8_t> disk;
    std::vector<uint8_t> image;
    uint32_t base = 0, imageSize = 0;
    size_t pe = 0, optional = 0, relocations = 0, relocationsEnd = 0;
    std::vector<Section> sections;
};

PeImage LoadPe(char const* path)
{
    // Only reads the operator-selected file. No engine load, process launch, or DLL execution.
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open configured PE file");
    PeImage peImage;
    peImage.disk.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    peImage.pe = Get(peImage.disk, 0x3c);
    peImage.optional = peImage.pe + 24;
    if (Short(peImage.disk, 0) != 0x5a4d ||
        Get(peImage.disk, peImage.pe) != 0x4550 ||
        Short(peImage.disk, peImage.pe + 4) != 0x14c ||
        Short(peImage.disk, peImage.optional) != 0x10b)
        throw std::runtime_error("configured file is not the expected PE32 image");
    peImage.base = Get(peImage.disk, peImage.optional + 28);
    peImage.imageSize = Get(peImage.disk, peImage.optional + 56);
    const uint32_t headers = Get(peImage.disk, peImage.optional + 60);
    if (peImage.imageSize < headers || peImage.imageSize > 128u * 1024 * 1024 ||
        !Range(peImage.disk.size(), 0, headers) ||
        !Range(peImage.imageSize, 0, headers))
        throw std::runtime_error("invalid configured PE image size or headers");
    peImage.image.assign(peImage.imageSize, 0);
    std::copy_n(peImage.disk.begin(), headers, peImage.image.begin());
    const size_t sectionTable = peImage.optional + Short(peImage.disk, peImage.pe + 20);
    const unsigned sectionCount = Short(peImage.disk, peImage.pe + 6);
    for (unsigned i = 0; i < sectionCount; ++i) {
        const size_t section = sectionTable + 40 * i;
        const uint32_t virtualSize = Get(peImage.disk, section + 8);
        const uint32_t at = Get(peImage.disk, section + 12);
        const uint32_t rawSize = Get(peImage.disk, section + 16);
        const uint32_t raw = Get(peImage.disk, section + 20);
        if (!Range(peImage.disk.size(), raw, rawSize) ||
            !Range(peImage.imageSize, at, rawSize))
            throw std::runtime_error("invalid configured PE section");
        std::copy_n(peImage.disk.begin() + raw, rawSize, peImage.image.begin() + at);
        const uint32_t mappedSize = virtualSize ? virtualSize : rawSize;
        if (!Range(peImage.imageSize, at, mappedSize))
            throw std::runtime_error("truncated configured PE section");
        peImage.sections.push_back({at, mappedSize, Get(peImage.disk, section + 36)});
    }
    peImage.relocations = Get(peImage.disk, peImage.optional + 136);
    peImage.relocationsEnd = peImage.relocations + Get(peImage.disk, peImage.optional + 140);
    if (!Range(peImage.imageSize, peImage.relocations, peImage.relocationsEnd - peImage.relocations) &&
        peImage.relocationsEnd != peImage.relocations)
        throw std::runtime_error("invalid configured PE relocation directory");
    return peImage;
}

void ApplyRelocations(std::vector<uint8_t>& image, PeImage const& pe, uint32_t delta)
{
    for (size_t block = pe.relocations; pe.relocationsEnd != pe.relocations && block < pe.relocationsEnd;) {
        if (!Range(image.size(), block, 8)) throw std::runtime_error("truncated PE relocation block");
        const uint32_t page = Get(image, block), size = Get(image, block + 4);
        if (size < 8 || !Range(pe.relocationsEnd, block, size))
            throw std::runtime_error("invalid PE relocation block");
        for (size_t at = block + 8; at < block + size; at += 2) {
            const uint16_t entry = Short(image, at);
            if (entry >> 12 == 3) {
                const size_t target = page + (entry & 0xfff);
                if (!Range(image.size(), target, 4)) throw std::runtime_error("invalid PE relocation target");
                Put(image, target, Get(image, target) + delta);
            } else if (entry >> 12 != 0) {
                throw std::runtime_error("unsupported PE relocation type");
            }
        }
        block += size;
    }
}

template<size_t N>
void CheckNamedExports(std::vector<uint8_t> const& image, size_t optional,
                       size_t const (&entry)[N], Site const (&catalog)[N])
{
    const size_t exports = Get(image, optional + 96);
    if (!exports) throw std::runtime_error("missing configured PE export directory");
    for (size_t i = 0; i < N; ++i) {
        if (!catalog[i].symbol) continue;
        unsigned found = 0;
        const uint32_t names = Get(image, exports + 32);
        const uint32_t ordinals = Get(image, exports + 36);
        const uint32_t functions = Get(image, exports + 28);
        const uint32_t nameCount = Get(image, exports + 24);
        for (unsigned n = 0; n < nameCount; ++n) {
            const size_t name = Get(image, names + n * 4);
            const size_t length = std::strlen(catalog[i].symbol) + 1;
            if (Range(image.size(), name, length) &&
                !std::memcmp(image.data() + name, catalog[i].symbol, length)) {
                const uint16_t ordinal = Short(image, ordinals + n * 2);
                if (Get(image, functions + ordinal * 4) == entry[i])
                    ++found;
            }
        }
        if (found != 1)
            throw std::runtime_error("configured native export mismatch");
    }
}

std::vector<size_t> CodeCalls(std::vector<uint8_t> const& image, PeImage const& pe, size_t target)
{
    std::vector<size_t> calls;
    for (auto const& section : pe.sections) {
        if (!(section.flags & 0x20000000) || !Range(image.size(), section.at, section.size)) continue;
        for (size_t at = section.at; at + 5 <= section.at + section.size; ++at)
            if (Calls(image.data(), image.size(), at, target)) calls.push_back(at);
    }
    return calls;
}

// HUD.SaveGame saves the file manager's retained image [manager+8]; only the menu-open handler and HUD.CaptureSaveImage
// (the quicksave message) write it. The mod relies on that: it never recaptures while the menu is drawn.
void CheckThumbnailOwners(std::vector<uint8_t> const& image, PeImage const& pe, size_t handleMessage)
{
    constexpr size_t CaptureSaveImage = 0xa9310, CreateImage = 0x19c500, SaveGame = 0xa9e90, SaveWorld = 0x19c6b0,
                     QuicksaveCall = 0x7dc0f, MenuShowCall = 0x8c3bd, IsMenuOpen = 0x7c2c0;
    constexpr char StoreRetained[] = "8b4e0885c98bf8740e8b018b5024ffd2c7460800000000897e08";
    const auto capture = CodeCalls(image, pe, CaptureSaveImage);
    if (capture.size() != 1 || capture[0] != QuicksaveCall || capture[0] <= handleMessage || capture[0] > handleMessage + 0x400)
        throw std::runtime_error("HUD.CaptureSaveImage is no longer called only by the GUI.HandleMessage quicksave branch");
    const auto create = CodeCalls(image, pe, CreateImage);
    if (create.size() != 2 || create[0] != MenuShowCall || create[1] != CaptureSaveImage + 0xc)
        throw std::runtime_error("the world scene image is no longer created only by menu show and the capture helper");
    for (size_t call : create)
        if (!Range(image.size(), call + 5, sizeof(StoreRetained)) ||
            !Matches(image.data() + call + 5, image.size() - call - 5, StoreRetained))
            throw std::runtime_error("scene image is no longer stored into [file manager+8] after creation");
    if (!Matches(image.data() + SaveGame + 0xa5, image.size() - SaveGame - 0xa5, "8b4d088b5108") ||
        !Calls(image.data(), image.size(), SaveGame + 0xb9, SaveWorld))
        throw std::runtime_error("HUD.SaveGame no longer passes the retained image [manager+8] to World.SaveGameWorld");
    if (!Matches(image.data() + IsMenuOpen, image.size() - IsMenuOpen, "8b410885c0740c33c9837848ff0f95c18ac1c332c0c3"))
        throw std::runtime_error("gCGUIManager::IsMenuOpen no longer reports the menu window state");
}

void CheckRealPe(std::vector<uint8_t> const& image, PeImage const& pe, uint32_t base)
{
    size_t sites[5]{};
    for (size_t i = 0; i < 5; ++i) {
        unsigned count = 0;
        sites[i] = Missing;
        if (i == 3) {
            for (auto const& section : pe.sections) {
                if (!(section.flags & 0x20000000)) continue;
                if (!Range(image.size(), section.at, section.size)) continue;
                unsigned found = 0;
                const size_t match = FindUnique(image.data() + section.at, section.size,
                                                 WorldLoadBody, found);
                if (match != Missing) sites[i] = section.at + match - Stolen;
                count += found;
            }
        } else {
            for (auto const& section : pe.sections) {
                if (!(section.flags & 0x20000000)) continue;
                if (!Range(image.size(), section.at, section.size)) continue;
                unsigned found = 0;
                const size_t match = FindUnique(image.data() + section.at, section.size,
                                                 Sites[i].bytes, found);
                if (match != Missing) sites[i] = section.at + match;
                count += found;
            }
        }
        if (count != 1 || sites[i] == Missing)
            throw std::runtime_error("configured native signature is not unique");
        if (sites[i] != Sites[i].hint)
            throw std::runtime_error("configured native signature resolved at the wrong RVA");
    }

    size_t ui[5]{};
    for (size_t i = 0; i < 5; ++i) {
        unsigned count = 0;
        ui[i] = Missing;
        for (auto const& section : pe.sections) {
            if (!(section.flags & 0x20000000) || !Range(image.size(), section.at, section.size)) continue;
            unsigned found = 0;
            const size_t match = FindUnique(image.data() + section.at, section.size, UiSites[i].bytes, found);
            if (match != Missing) ui[i] = section.at + match;
            count += found;
        }
        if (count != 1 || ui[i] != UiSites[i].hint)
            throw std::runtime_error("configured save UI/native availability signature mismatch");
        auto corrupt = image;
        const auto pattern = Decode(UiSites[i].bytes);
        for (size_t at = 0; at < pattern.size(); ++at) {
            if (pattern[at] < 0) continue;
            corrupt[ui[i] + at] ^= 1;
            assert(!Matches(corrupt.data() + ui[i], corrupt.size() - ui[i], UiSites[i].bytes));
            corrupt[ui[i] + at] ^= 1;
        }
        assert(!Matches(image.data() + ui[i], pattern.size() - 1, UiSites[i].bytes));
    }
    if (!Calls(image.data(), image.size(), ui[2], sites[4]) ||
        !Calls(image.data(), image.size(), ui[2] + 6, ui[1]) ||
        !Calls(image.data(), image.size(), ui[1] + 0x27, sites[0]))
        throw std::runtime_error("configured menu save call edge mismatch");
    static_assert(GuiStolen == 7);
    assert(Matches(image.data() + ui[0], GuiStolen, "8b44240483ec68"));
    CheckNamedExports(image, pe.optional, sites, Sites);
    CheckNamedExports(image, pe.optional, ui, UiSites);
    unsigned deletionCount = 0;
    size_t deletion = Missing;
    for (auto const& section : pe.sections) {
        if (!(section.flags & 0x20000000) || !Range(image.size(), section.at, section.size)) continue;
        unsigned found = 0;
        const size_t at = FindUnique(image.data() + section.at, section.size, DeleteSite.bytes, found);
        if (at != Missing) deletion = section.at + at;
        deletionCount += found;
    }
    if (deletionCount != 1 || deletion != DeleteSite.hint ||
        !Matches(image.data() + deletion, Stolen, "83ec10535657"))
        throw std::runtime_error("configured native slot deletion signature mismatch");
    const size_t deletionEntries[1]{deletion};
    const Site deletionSites[1]{DeleteSite};
    CheckNamedExports(image, pe.optional, deletionEntries, deletionSites);

    const std::vector<int> original = Decode(Sites[3].bytes);
    if (!Range(image.size(), sites[3], original.size()) ||
        !Matches(image.data() + sites[3], image.size() - sites[3], Sites[3].bytes))
        throw std::runtime_error("configured WorldLoad entry is modified or not intact");

    size_t owner = 0, table = 0;
    if (!Links(image.data(), image.size(), base, sites, owner, table))
        throw std::runtime_error("configured HUD/world/call edges or getter operands changed");
    if (owner != 0x403c20u || table != 0x2d9aa0u)
        throw std::runtime_error("configured singleton or vtable operand mismatch");
    CheckThumbnailOwners(image, pe, ui[0]);
}

void CheckReal(char const* gamePath, char const* pupPath)
{
    PeImage pe = LoadPe(gamePath);
    CheckRealPe(pe.image, pe, pe.base);

    const uint32_t upward = pe.base + 0x21000000u;
    std::vector<uint32_t> bases{pe.base, 0x01f90000u, upward, 0x00ea0000u};
    for (uint32_t actual : bases) {
        std::vector<uint8_t> relocated = pe.image;
        const uint32_t delta = actual - pe.base;
        ApplyRelocations(relocated, pe, delta);
        CheckRealPe(relocated, pe, actual);
    }
    (void)pupPath;
}

void CheckPup(char const* pupPath)
{
    PeImage pe = LoadPe(pupPath);
    size_t initialCallback = Missing;
    PupLayout initialLayout{};
    for (uint32_t actual : {pe.base, 0x724b0000u,
                            0x724b0000u + 0x21000000u,
                            0x724b0000u - 0x21000000u}) {
        if (actual > UINT32_MAX - pe.imageSize)
            throw std::runtime_error("configured PUP image address overflows");
        std::vector<uint8_t> relocated = pe.image;
        ApplyRelocations(relocated, pe, actual - pe.base);
        unsigned callbackMatches = 0;
        size_t callback = Missing;
        for (auto const& section : pe.sections) {
            if (!(section.flags & 0x20000000)) continue;
            if (!Range(relocated.size(), section.at, section.size)) continue;
            unsigned found = 0;
            const size_t match = FindUnique(relocated.data() + section.at, section.size,
                                             PupReceiverBytes, found);
            if (match != Missing) callback = section.at + match;
            callbackMatches += found;
        }
        if (callbackMatches != 1)
            throw std::runtime_error("configured PUP callback is missing or ambiguous");
        PupLayout layout{};
        if (!PupReceiverLayoutAt(relocated.data() + callback, relocated.size() - callback,
                                 actual + static_cast<uint32_t>(callback), actual, pe.imageSize, layout))
            throw std::runtime_error("configured PUP callback owner/address validation mismatch");
        if (PupReceiverLayoutAt(relocated.data() + callback, relocated.size() - callback,
                                actual + pe.imageSize, actual, pe.imageSize, layout))
            throw std::runtime_error("configured PUP callback accepted an outside address");
        if (PupReceiverLayoutAt(relocated.data() + callback, relocated.size() - callback,
                                actual + static_cast<uint32_t>(callback), actual + pe.imageSize, pe.imageSize, layout))
            throw std::runtime_error("configured PUP callback accepted a foreign owner");
        if (initialCallback == Missing) {
            initialCallback = callback;
            initialLayout = layout;
        } else if (callback != initialCallback || layout.semaphore != initialLayout.semaphore ||
                   layout.waitImport != initialLayout.waitImport || layout.releaseImport != initialLayout.releaseImport ||
                   layout.adapter != initialLayout.adapter) {
            throw std::runtime_error("configured PUP callback relocations changed its layout");
        }
    }
    std::cout << "pup digest=" << Sha256(pe.disk) << " verified="
              << (PupLayoutIsPinned(static_cast<uint32_t>(initialCallback), pe.imageSize, initialLayout)
                      ? "pinned" : "structural") << '\n';
}
}

int main(int argc, char** argv)
{
    Fixture();
    PairedSaveFixtures();
    StructuralReceiverFixtures();
    char const* game = nullptr;
    char const* pup = nullptr;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--game")) game = argv[i + 1];
        else if (!std::strcmp(argv[i], "--pup")) pup = argv[i + 1];
        else throw std::runtime_error("unknown native fixture option");
    }
    if (game) CheckReal(game, pup);
    if (pup) CheckPup(pup);
}
