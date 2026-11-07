// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/saving_native.h"
#include <algorithm>
#include <cassert>
#include <fstream>
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
    Put(callback, 7, pupBase + 0x99510);
    Put(callback, 13, pupBase + 0x677a0);
    Put(callback, 23, pupBase + 0x99510);
    Put(callback, 29, pupBase + 0x677a4);
    Put(callback, 34, pupBase + 0x97578);
    assert(PupReceiver(callback.data(), callback.size(), pupBase));
    return callback;
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
    const uint32_t callbackAddress = fixture.pupBase + PupReceiverRva;
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
    assert(PupReceiver(callbackBytes.data(), callbackBytes.size(), fixture.pupBase));
    assert(PupReceiverAt(callbackBytes.data(), callbackBytes.size(),
                         callbackAddress, fixture.pupBase, PupImageSize));
    assert(!PupReceiverAt(callbackBytes.data(), callbackBytes.size(),
                          0x720c57f0u, fixture.pupBase, PupImageSize));
    assert(!PupReceiverAt(callbackBytes.data(), callbackBytes.size(),
                          callbackAddress, fixture.pupBase + 1, PupImageSize));
    assert(!PupReceiverAt(callbackBytes.data(), callbackBytes.size(),
                          callbackAddress, fixture.pupBase, PupImageSize - 4));

    auto foreign = receiver;
    assert(!PupReceiver(foreign.data(), foreign.size(), fixture.pupBase + 1));

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
    assert(!PupReceiver(truncatedReceiver.data(), 40, fixture.pupBase));
    AddressReader truncatedReceiverReader;
    truncatedReceiver.pop_back();
    assert(truncatedReceiverReader.Put(callbackAddress, truncatedReceiver));
    assert(!truncatedReceiverReader.Read(callbackAddress, 41, callbackBytes));
    callbackBytes = receiver;
    for (size_t at : {0u, 6u, 12u, 22u, 28u, 33u, 40u}) {
        auto corruptFixed = callbackBytes;
        corruptFixed[at] ^= 1;
        assert(!PupReceiver(corruptFixed.data(), corruptFixed.size(), fixture.pupBase));
    }
    for (size_t operand : {7u, 13u, 23u, 29u, 34u}) {
        auto corruptOperand = callbackBytes;
        Put(corruptOperand, operand, Get(corruptOperand, operand) + 1);
        assert(!PupReceiver(corruptOperand.data(), corruptOperand.size(), fixture.pupBase));
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
    std::vector<uint8_t> callback(PupImageSize, 0xcc);
    const size_t callbackRva = PupReceiverRva;
    const auto receiver = Decode(PupReceiverBytes);
    assert(receiver.size() == 41);
    for (size_t i = 0; i < receiver.size(); ++i)
        callback[callbackRva + i] = receiver[i] < 0 ? 0 : static_cast<uint8_t>(receiver[i]);
    Put(callback, callbackRva + 7, pupBase + 0x99510);
    Put(callback, callbackRva + 13, pupBase + 0x677a0);
    Put(callback, callbackRva + 23, pupBase + 0x99510);
    Put(callback, callbackRva + 29, pupBase + 0x677a4);
    Put(callback, callbackRva + 34, pupBase + 0x97578);
    assert(PupReceiver(callback.data() + callbackRva, callback.size() - callbackRva, pupBase));
    assert(!PupReceiver(callback.data() + callbackRva, 40, pupBase));
    for (size_t at : {callbackRva, callbackRva + 6, callbackRva + 12,
                      callbackRva + 22, callbackRva + 28, callbackRva + 33, callbackRva + 40}) {
        auto corrupt = callback;
        corrupt[at] ^= 1;
        assert(!PupReceiver(corrupt.data() + callbackRva, corrupt.size() - callbackRva, pupBase));
    }
    for (size_t operand : {7u, 13u, 23u, 29u, 34u}) {
        auto corrupt = callback;
        Put(corrupt, callbackRva + operand, Get(corrupt, callbackRva + operand) + 1);
        assert(!PupReceiver(corrupt.data() + callbackRva, corrupt.size() - callbackRva, pupBase));
    }
    assert(!PupReceiver(callback.data() + callbackRva,
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
    assert(PupReceiver(callback.data() + callbackRva,
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

void CheckNamedExports(std::vector<uint8_t> const& image, size_t optional,
                       size_t const (&entry)[5])
{
    const size_t exports = Get(image, optional + 96);
    if (!exports) throw std::runtime_error("missing configured PE export directory");
    for (size_t i = 2; i < 4; ++i) {
        unsigned found = 0;
        const uint32_t names = Get(image, exports + 32);
        const uint32_t ordinals = Get(image, exports + 36);
        const uint32_t functions = Get(image, exports + 28);
        const uint32_t nameCount = Get(image, exports + 24);
        for (unsigned n = 0; n < nameCount; ++n) {
            const size_t name = Get(image, names + n * 4);
            const size_t length = std::strlen(Sites[i].symbol) + 1;
            if (Range(image.size(), name, length) &&
                !std::memcmp(image.data() + name, Sites[i].symbol, length)) {
                const uint16_t ordinal = Short(image, ordinals + n * 2);
                if (Get(image, functions + ordinal * 4) == entry[i])
                    ++found;
            }
        }
        if (found != 1)
            throw std::runtime_error("configured world export mismatch");
    }
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

    CheckNamedExports(image, pe.optional, sites);

    const std::vector<int> original = Decode(Sites[3].bytes);
    if (!Range(image.size(), sites[3], original.size()) ||
        !Matches(image.data() + sites[3], image.size() - sites[3], Sites[3].bytes))
        throw std::runtime_error("configured WorldLoad entry is modified or not intact");

    size_t owner = 0, table = 0;
    if (!Links(image.data(), image.size(), base, sites, owner, table))
        throw std::runtime_error("configured HUD/world/call edges or getter operands changed");
    if (owner != 0x403c20u || table != 0x2d9aa0u)
        throw std::runtime_error("configured singleton or vtable operand mismatch");
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

uint32_t CheckPup(char const* pupPath)
{
    PeImage pe = LoadPe(pupPath);
    if (pe.imageSize != PupImageSize)
        throw std::runtime_error("configured PUP image size mismatch");

    for (uint32_t actual : {pe.base, 0x724b0000u,
                            0x724b0000u + 0x21000000u,
                            0x724b0000u - 0x21000000u}) {
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
        if (callbackMatches != 1 || callback != PupReceiverRva)
            throw std::runtime_error("configured PUP callback is missing or ambiguous");
        if (!PupReceiver(relocated.data() + PupReceiverRva,
                         relocated.size() - PupReceiverRva, actual))
            throw std::runtime_error("configured PUP callback operands mismatch");
        if (!PupReceiverAt(relocated.data() + PupReceiverRva,
                           relocated.size() - PupReceiverRva,
                           actual + PupReceiverRva, actual, PupImageSize))
            throw std::runtime_error("configured PUP callback owner/address validation mismatch");
        if (PupReceiverAt(relocated.data() + PupReceiverRva,
                          relocated.size() - PupReceiverRva,
                          actual + PupReceiverRva + 1, actual, PupImageSize))
            throw std::runtime_error("configured PUP callback accepted a wrong RVA");
        if (PupReceiverAt(relocated.data() + PupReceiverRva,
                          relocated.size() - PupReceiverRva,
                          actual + PupReceiverRva, actual + 1, PupImageSize))
            throw std::runtime_error("configured PUP callback accepted a foreign owner");
        if (PupReceiverAt(relocated.data() + PupReceiverRva,
                          relocated.size() - PupReceiverRva,
                          actual + PupReceiverRva, actual, PupImageSize - 4))
            throw std::runtime_error("configured PUP callback accepted the wrong module image size");
    }
    return 0x724b0000u;
}
}

int main(int argc, char** argv)
{
    Fixture();
    PairedSaveFixtures();
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
