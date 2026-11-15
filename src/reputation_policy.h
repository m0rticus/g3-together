// SPDX-License-Identifier: GPL-3.0-or-later
// Pure policy and wire helpers for host-authoritative camp (gCPlayerMemory_PS::m_arrPoliticalFame)
// and enclave (gCEnclave_PS::m_i32PlayerFame) reputation synchronization.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace g3t::reputation {

constexpr size_t PoliticalAlignmentCount = 15;     // gEPoliticalAlignment 0..14 (None..Gonzalez)
constexpr size_t MinNativePoliticalArraySize = 10; // Game.dll OnPostRead (0x1014e8e0) & Script.dll (0x10010200)
constexpr int32_t MinClampedFame = 0;
constexpr int32_t MaxClampedFame = 100;
constexpr int32_t LiberationFame = 100;

// Lossless int32_t <-> uint32_t ZigZag encoding preserving 0 <-> 0u (the New Game default).
inline uint32_t EncodeFame(int32_t fame)
{
    const uint32_t u = static_cast<uint32_t>(fame);
    return (u << 1) ^ static_cast<uint32_t>(fame >> 31);
}

inline int32_t DecodeFame(uint32_t encoded)
{
    return static_cast<int32_t>((encoded >> 1) ^ (~(encoded & 1u) + 1u));
}

inline bool ParseUnsigned(std::string const& text, uint32_t& out)
{
    if (text.empty() || text.size() > 10 || (text.size() > 1 && text[0] == '0')) return false;
    uint32_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(c - '0');
        if (value > ((std::numeric_limits<uint32_t>::max)() - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    out = value;
    return true;
}

inline std::string PoliticalKey(size_t alignment)
{
    return "@pol/" + std::to_string(alignment);
}

inline std::string EnclaveKey(std::string const& enclaveName)
{
    return "@enc/" + enclaveName;
}

inline bool ParseKey(std::string const& key, bool& isPolitical, size_t& alignment, std::string& enclaveName)
{
    if (key.compare(0, 5, "@pol/") == 0) {
        uint32_t idx = 0;
        if (!ParseUnsigned(key.substr(5), idx) || idx >= PoliticalAlignmentCount) return false;
        isPolitical = true;
        alignment = static_cast<size_t>(idx);
        enclaveName.clear();
        return true;
    }
    if (key.compare(0, 5, "@enc/") == 0) {
        const std::string name = key.substr(5);
        if (name.empty() || name.size() > 31) return false;
        for (char c : name) {
            if (c == '\0' || c == '\x1f') return false;
        }
        isPolitical = false;
        alignment = 0;
        enclaveName = name;
        return true;
    }
    return false;
}

inline bool PackState(std::string const& key, uint32_t encoded, size_t limit, std::string& out)
{
    bool isPolitical = false;
    size_t alignment = 0;
    std::string enclaveName;
    if (!ParseKey(key, isPolitical, alignment, enclaveName)) return false;
    const std::string packed = key + '\x1f' + std::to_string(encoded);
    if (packed.size() >= limit) return false;
    out = packed;
    return true;
}

inline bool UnpackState(std::string const& packed, std::string& key, uint32_t& encoded)
{
    const size_t split = packed.rfind('\x1f');
    if (split == std::string::npos || split == 0 || split + 1 >= packed.size()) return false;
    const std::string candidate = packed.substr(0, split);
    bool isPolitical = false;
    size_t alignment = 0;
    std::string enclaveName;
    uint32_t parsed = 0;
    if (!ParseKey(candidate, isPolitical, alignment, enclaveName) ||
        !ParseUnsigned(packed.substr(split + 1), parsed)) return false;
    key = candidate;
    encoded = parsed;
    return true;
}

// Ensures Script.dll PSPlayerMemory::GetPoliticalFame/SetPoliticalFame (0..9 unchecked) and
// Game.dll gCQuest_PS::SetStatus (< m_u32Count) stay safe without needlessly growing 10-slot arrays for 0s.
inline size_t RequiredPoliticalArrayCount(size_t currentCount, size_t index, int32_t value)
{
    const size_t base = (std::max)(currentCount, MinNativePoliticalArraySize);
    if (index >= PoliticalAlignmentCount) return base;
    if (value == 0 && index >= base) return base;
    return (std::max)(base, index + 1);
}

inline int32_t ApplyClampedDelta(int32_t current, int32_t delta)
{
    const int64_t next = static_cast<int64_t>(current) + static_cast<int64_t>(delta);
    if (next < MinClampedFame) return MinClampedFame;
    if (next > MaxClampedFame) return MaxClampedFame;
    return static_cast<int32_t>(next);
}

inline bool EqualsIgnoreCaseAscii(char const* a, char const* b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        const unsigned char ca = static_cast<unsigned char>(*a++);
        const unsigned char cb = static_cast<unsigned char>(*b++);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return *a == '\0' && *b == '\0';
}

// Maps Script_Game.dll IS_Running_ProcessInfo (0x100b7f10) ModReputPol ID1 tokens to gEPoliticalAlignment.
inline int PoliticalAlignmentFromToken(char const* token)
{
    if (!token || !*token) return -1;
    if (std::strcmp(token, "Orc") == 0) return 1;
    if (std::strcmp(token, "Nrd") == 0) return 2;
    if (std::strcmp(token, "Reb") == 0) return 3;
    if (std::strcmp(token, "Mid") == 0) return 4;
    if (std::strcmp(token, "Ass") == 0) return 5;
    if (std::strcmp(token, "Nom") == 0) return 6;
    if (std::strcmp(token, "Pir") == 0) return 9;
    return -1;
}

struct DialogRepMutation {
    bool isPolitical = false;
    int alignment = -1;
    std::string enclave;
    int32_t delta = 0;
};

// Parses Script_Game.dll IS_Running_ProcessInfo cases 0x3b (ModReputPol) and 0x3c (ModReputEnc).
inline bool ParseDialogReputationCommand(char const* command, char const* id1, char const* id2,
                                         char const* textId, char const* entity2Name, DialogRepMutation& out)
{
    if (!command || !id2 || !*id2) return false;
    const bool pol = EqualsIgnoreCaseAscii(command, "ModReputPol");
    const bool enc = !pol && EqualsIgnoreCaseAscii(command, "ModReputEnc");
    if (!pol && !enc) return false;
    char const* p = id2;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '+') ++p;
    if (*p < '0' || *p > '9') return false;
    int64_t amount = 0;
    while (*p >= '0' && *p <= '9') {
        amount = amount * 10 + (*p - '0');
        if (amount > MaxClampedFame) amount = MaxClampedFame + 1;
        ++p;
    }
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '\0' || amount <= 0) return false;
    const int32_t clampedAmount = static_cast<int32_t>((std::min<int64_t>)(amount, MaxClampedFame));
    const bool subtract = textId && std::strcmp(textId, "-") == 0;
    const int32_t signedDelta = subtract ? -clampedAmount : clampedAmount;
    if (pol) {
        const int align = PoliticalAlignmentFromToken(id1);
        if (align < 0) return false;
        out = {true, align, {}, signedDelta};
        return true;
    }
    if (!entity2Name || !*entity2Name || std::strlen(entity2Name) > 31) return false;
    out = {false, -1, entity2Name, signedDelta};
    return true;
}

// Matches Script_Game.dll ChangeEnclaveStatus (0x100602d0): transitioning to Routine (0) from Attack (1) or Flee (2)
// sets PlayerFame = 100 and clears enclave crime state.
inline bool ShouldAwardLiberationFame(int oldStatus, int newStatus)
{
    return oldStatus != newStatus && newStatus == 0;
}

struct RequestTracker {
    bool active = false;
    uint32_t welcome = 0;
    uint32_t epoch = 0;
};

// Prevents pre-admission EnclaveRequest drops (!inWorld) and re-requests on reconnect or host save/load.
inline bool UpdateSessionRequest(RequestTracker& tracker, bool connected, bool inWorld, bool gameRunning,
                                 uint32_t welcomeSerial, uint32_t worldEpoch)
{
    if (!connected || !inWorld || !gameRunning) {
        tracker.active = false;
        return false;
    }
    if (!tracker.active || tracker.welcome != welcomeSerial || tracker.epoch != worldEpoch) {
        tracker.active = true;
        tracker.welcome = welcomeSerial;
        tracker.epoch = worldEpoch;
        return true;
    }
    return false;
}

} // namespace g3t::reputation
