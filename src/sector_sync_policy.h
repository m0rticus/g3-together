// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-free policy and wire/extension codecs for host-authoritative world sector synchronization.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace g3t::sector_sync {

constexpr uint8_t WorldStateKind = 2;
constexpr uint8_t FlagRequest = 0x80;
constexpr uint8_t FlagBaseline = 0x40;
constexpr uint8_t FlagEnd = 0x20;
constexpr uint8_t FlagCatchup = 0x10;
constexpr uint8_t HandshakeMask = 0xf0;

constexpr uint32_t ExtensionType = 257; // saving::store::Extension + 1
constexpr uint32_t ExtensionVersion = 1;
constexpr size_t MaxExtensionBytes = 1024u * 1024u;
constexpr size_t MaxWireNameLen = 47;

using StateMap = std::map<std::string, bool>;
using DeltaList = std::vector<std::pair<std::string, bool>>;

inline bool IsHandshakeWire(uint8_t kind, uint8_t npcClass, char const* routine) noexcept
{
    if (kind != WorldStateKind) return false;
    const bool hasName = routine && routine[0] != '\0';
    if (npcClass == FlagRequest) return !hasName;
    if (npcClass == FlagBaseline) return true; // Begin has empty routine; entries have non-empty routine.
    if (npcClass == (FlagBaseline | FlagEnd)) return !hasName;
    if (npcClass == (FlagBaseline | FlagCatchup)) return hasName;
    return false;
}

inline bool ValidSendWorldStateTo(bool isHost, uint8_t kind, uint8_t npcClass, char const* routine) noexcept
{
    if (!IsHandshakeWire(kind, npcClass, routine)) return false;
    return isHost ? ((npcClass & FlagBaseline) != 0) : (npcClass == FlagRequest);
}

inline void PackCount(uint8_t guid[16], uint32_t count) noexcept
{
    std::memset(guid, 0, 16);
    guid[0] = static_cast<uint8_t>(count & 0xffu);
    guid[1] = static_cast<uint8_t>((count >> 8) & 0xffu);
    guid[2] = static_cast<uint8_t>((count >> 16) & 0xffu);
    guid[3] = static_cast<uint8_t>((count >> 24) & 0xffu);
}

inline uint32_t UnpackCount(uint8_t const guid[16]) noexcept
{
    return static_cast<uint32_t>(guid[0]) | (static_cast<uint32_t>(guid[1]) << 8) |
           (static_cast<uint32_t>(guid[2]) << 16) | (static_cast<uint32_t>(guid[3]) << 24);
}

inline DeltaList DiffStates(StateMap const& before, StateMap const& current)
{
    DeltaList delta;
    for (auto const& [name, enabled] : current) {
        auto it = before.find(name);
        if (it == before.end() || it->second != enabled) delta.emplace_back(name, enabled);
    }
    return delta;
}

struct HostState {
    struct Replay {
        uint32_t generation = 0;
        bool captured = false;
        bool sent = false;
        bool finished = false;
        StateMap values;
        StateMap latest;
    };

    bool seeded = false;
    StateMap known;
    std::array<Replay, 8> replays{};

    void Reset()
    {
        seeded = false;
        known.clear();
        replays = {};
    }

    void Seed(StateMap const& current)
    {
        known = current;
        seeded = true;
    }

    bool RecordLive(std::string const& name, bool enabled)
    {
        if (!seeded || name.empty()) return false;
        auto it = known.find(name);
        if (it != known.end() && it->second == enabled) return false;
        known[name] = enabled;
        return true;
    }

    DeltaList Reconcile(StateMap const& current)
    {
        if (!seeded) {
            Seed(current);
            return {};
        }
        DeltaList changed = DiffStates(known, current);
        for (auto const& [name, enabled] : changed) known[name] = enabled;
        return changed;
    }

    StateMap const* BeginReplay(uint8_t peer, uint32_t generation, StateMap const& current)
    {
        if (peer < 2 || peer >= replays.size() || !generation) return nullptr;
        Replay& r = replays[peer];
        if (r.generation != generation || r.finished) {
            r = Replay{};
            r.generation = generation;
        }
        if (!r.captured) {
            r.values = current;
            r.latest = current;
            r.captured = true;
        }
        return &r.values;
    }

    void MarkReplaySent(uint8_t peer, uint32_t generation)
    {
        if (peer < 2 || peer >= replays.size()) return;
        Replay& r = replays[peer];
        if (r.generation == generation && r.captured) r.sent = true;
    }

    bool FinishReplay(uint8_t peer, uint32_t generation, StateMap const& current, DeltaList& outCatchup)
    {
        outCatchup.clear();
        if (peer < 2 || peer >= replays.size()) return false;
        Replay& r = replays[peer];
        if (!r.captured || !r.sent || r.generation != generation) return false;
        outCatchup = DiffStates(r.latest, current);
        r.latest = current;
        r.finished = true;
        return true;
    }
};

struct ClientState {
    bool admissionStarted = false;
    bool admissionApplied = false;
    bool begun = false;
    bool complete = false;
    uint32_t expectedCount = 0;
    uint32_t receivedCount = 0;
    StateMap baseline;
    StateMap pendingLive;
    StateMap known;

    void Reset()
    {
        admissionStarted = false;
        admissionApplied = false;
        begun = false;
        complete = false;
        expectedCount = 0;
        receivedCount = 0;
        baseline.clear();
        pendingLive.clear();
        known.clear();
    }

    void PrepareAdmission()
    {
        admissionStarted = true;
        admissionApplied = false;
        begun = false;
        complete = false;
        expectedCount = 0;
        receivedCount = 0;
        baseline.clear();
        pendingLive.clear();
    }

    bool OnBaselineMessage(uint8_t npcClass, char const* routine, bool enabled, uint32_t countHint)
    {
        if (!admissionStarted) return false;
        const bool hasName = routine && routine[0] != '\0';
        if (npcClass == FlagBaseline) {
            if (!hasName) {
                if (complete) return false;
                baseline.clear();
                begun = true;
                complete = false;
                expectedCount = 0;
                receivedCount = 0;
                return true;
            }
            if (!begun || complete) return false;
            baseline[routine] = enabled;
            ++receivedCount;
            return true;
        }
        if (npcClass == (FlagBaseline | FlagEnd)) {
            if (!begun || complete || hasName) return false;
            if (receivedCount != countHint || baseline.size() != receivedCount) {
                baseline.clear();
                begun = false;
                receivedCount = 0;
                return false;
            }
            expectedCount = countHint;
            complete = true;
            return true;
        }
        if (npcClass == (FlagBaseline | FlagCatchup)) {
            if (!hasName) return false;
            if (!admissionApplied) pendingLive[routine] = enabled;
            else known[routine] = enabled;
            return true;
        }
        return false;
    }

    void OnLiveDelta(std::string const& name, bool enabled)
    {
        if (name.empty()) return;
        if (!admissionApplied) pendingLive[name] = enabled;
        else known[name] = enabled;
    }

    bool TakeCommitMap(StateMap& outStates)
    {
        if (!admissionStarted || !complete || admissionApplied) return false;
        outStates = baseline;
        for (auto const& [name, enabled] : pendingLive) outStates[name] = enabled;
        known = outStates;
        pendingLive.clear();
        admissionApplied = true;
        return true;
    }
};

inline bool EncodeExtension(StateMap const& states, std::vector<uint8_t>& out)
{
    out.clear();
    static constexpr uint8_t Magic[8] = {'G', '3', 'T', 'S', 'E', 'C', '0', '1'};
    out.insert(out.end(), Magic, Magic + 8);
    const uint32_t count = static_cast<uint32_t>(states.size());
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((count >> (8 * i)) & 0xffu));
    for (auto const& [name, enabled] : states) {
        if (name.empty() || name.size() > 255 || name.find('\0') != std::string::npos) {
            out.clear();
            return false;
        }
        const uint16_t len = static_cast<uint16_t>(name.size());
        out.push_back(enabled ? 1 : 0);
        out.push_back(static_cast<uint8_t>(len & 0xffu));
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xffu));
        out.insert(out.end(), name.begin(), name.end());
        if (out.size() > MaxExtensionBytes) {
            out.clear();
            return false;
        }
    }
    return true;
}

inline bool DecodeExtension(uint32_t savedVersion, std::vector<uint8_t> const& bytes, StateMap& out, bool& outPresent)
{
    out.clear();
    if (savedVersion == 0 && bytes.empty()) {
        outPresent = false;
        return true;
    }
    if (savedVersion != ExtensionVersion || bytes.size() < 12 || bytes.size() > MaxExtensionBytes ||
        std::memcmp(bytes.data(), "G3TSEC01", 8) != 0)
        return false;
    uint32_t count = 0;
    for (int i = 0; i < 4; ++i) count |= static_cast<uint32_t>(bytes[8 + i]) << (8 * i);
    size_t at = 12;
    for (uint32_t i = 0; i < count; ++i) {
        if (bytes.size() - at < 3) return false;
        const uint8_t enabled = bytes[at];
        const uint16_t len = static_cast<uint16_t>(bytes[at + 1]) | (static_cast<uint16_t>(bytes[at + 2]) << 8);
        at += 3;
        if (enabled > 1 || len == 0 || len > 255 || bytes.size() - at < len) return false;
        std::string name(reinterpret_cast<char const*>(bytes.data() + at), len);
        at += len;
        if (name.find('\0') != std::string::npos) return false;
        if (!out.emplace(std::move(name), enabled != 0).second) return false;
    }
    if (at != bytes.size()) return false;
    outPresent = true;
    return true;
}

} // namespace g3t::sector_sync
