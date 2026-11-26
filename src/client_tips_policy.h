// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-free policy and native OpenTutorial layout contract for enabling
// Gothic 3's "Do not show tips" toggle by default on client join/world entry.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace g3t::tips::policy {

constexpr uintptr_t OpenTutorialRva = 0x7c4b0;
constexpr size_t GuiRootOffset = 0x04;
constexpr size_t TutorialPageOffset = 0x74;
constexpr size_t TutorialCheckBoxOffset = 0x58;
constexpr size_t OpenTutorialProbeBytes = 29;

inline bool VerifyOpenTutorialBytes(uint8_t const* code, size_t size, uint32_t& outGetCheckIat) noexcept
{
    // gCGUIManager::OpenTutorial (Game.dll + 0x7c4b0):
    //   push esi; mov esi,[ecx+4]; test esi,esi; jz +0x36;
    //   push edi; mov edi,[esi+0x74]; test edi,edi; jz +0x27;
    //   lea ecx,[edi+0x58]; call [IAT CFFGFCCheckBox::GetCheck]; test al,al; jnz +0x1a
    static constexpr uint8_t Prefix[] = {
        0x56, 0x8b, 0x71, 0x04, 0x85, 0xf6, 0x74, 0x36,
        0x57, 0x8b, 0x7e, 0x74, 0x85, 0xff, 0x74, 0x27,
        0x8d, 0x4f, 0x58, 0xff, 0x15
    };
    static constexpr uint8_t Suffix[] = {0x84, 0xc0, 0x75, 0x1a};
    outGetCheckIat = 0;
    if (!code || size < sizeof(Prefix) + 4 + sizeof(Suffix)) return false;
    if (std::memcmp(code, Prefix, sizeof(Prefix)) != 0) return false;
    if (std::memcmp(code + sizeof(Prefix) + 4, Suffix, sizeof(Suffix)) != 0) return false;
    std::memcpy(&outGetCheckIat, code + sizeof(Prefix), sizeof(outGetCheckIat));
    return outGetCheckIat != 0;
}

struct State {
    bool isHost = true;
    bool joined = false;
    bool inRunningWorld = false;
    uint32_t welcomeSerial = 0;
    uint32_t worldEntrySeq = 0;
    uintptr_t heroToken = 0;
};

class JoinDefaultTracker {
    bool applied_ = false;
    uint32_t welcomeSerial_ = 0;
    uint32_t worldEntrySeq_ = 0;
    uintptr_t heroToken_ = 0;

  public:
    static bool ShouldPrepareEntry(bool isHost, bool joined) noexcept
    {
        return !isHost && joined;
    }

    bool ShouldApply(State const& s) noexcept
    {
        if (s.isHost || !s.joined) {
            Reset();
            return false;
        }
        if (!s.inRunningWorld || !s.heroToken) return false;
        if (!applied_) return true;
        return s.welcomeSerial != welcomeSerial_ || s.worldEntrySeq != worldEntrySeq_ || s.heroToken != heroToken_;
    }

    void MarkApplied(State const& s) noexcept
    {
        if (s.isHost || !s.joined || !s.inRunningWorld || !s.heroToken) return;
        applied_ = true;
        welcomeSerial_ = s.welcomeSerial;
        worldEntrySeq_ = s.worldEntrySeq;
        heroToken_ = s.heroToken;
    }

    void Reset() noexcept
    {
        applied_ = false;
        welcomeSerial_ = 0;
        worldEntrySeq_ = 0;
        heroToken_ = 0;
    }

    bool Applied() const noexcept { return applied_; }
};

} // namespace g3t::tips::policy
