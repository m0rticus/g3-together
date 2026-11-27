// SPDX-License-Identifier: GPL-3.0-or-later
// Host main-thread frame-time breakdown: separates Gothic 3 native engine + GPU/OS frame time (and foreground window
// focus) from G3Together module time (`world`, `remote / ENet`, `npcs`, and total `Module::Process`). Pure passive
// observation; never delays, caps, throttles, or skips any gameplay or network work.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace g3t::host_frame_diag {

constexpr uint32_t HeartbeatPeriodMs = 5000;
constexpr std::size_t TopPhaseCount = 3;

enum class Phase : uint8_t {
    World,
    Net,
    Npcs,
};

enum class SubPhase : uint8_t {
    Frame = 0,
    Crashlog,
    NpcRemoval,
    NpcIdentity,
    NpcPlayback,
    Ai,
    Combat,
    Summon,
    Story,
    Talk,
    Downed,
    Range,
    Partner,
    Doors,
    Sectors,
    Enclaves,
    Journal,
    Persist,
    Clock,
    InfoCmd,
    Guide,
    Arena,
    Ending,
    Containers,
    Trade,
    MapMarks,
    Ransack,
    Witness,
    Guards,
    Pickpocket,
    Respawn,
    Occupancy,
    Address,
    FightMem,
    Relation,
    XpShare,
    DialogAudio,
    Offer,
    Shot,
    Spells,
    Telemetry,
    PersistEnd,
    Hud,
    Count
};

constexpr std::size_t SubPhaseCount = static_cast<std::size_t>(SubPhase::Count);

inline char const* SubPhaseName(SubPhase phase) noexcept
{
    static constexpr std::array<char const*, SubPhaseCount> names = {
        "frame", "crashlog", "npc_removal", "npc_identity", "npc_playback", "ai", "combat",
        "summon", "story", "talk", "downed", "range", "partner", "doors", "sectors",
        "enclaves", "journal", "persist", "clock", "infocmd", "guide", "arena", "ending",
        "containers", "trade", "mapmarks", "ransack", "witness", "guards", "pickpocket",
        "respawn", "occupancy", "address", "fightmem", "relation", "xpshare", "dialogaudio",
        "offer", "shot", "spells", "telemetry", "persist_end", "hud",
    };
    const std::size_t idx = static_cast<std::size_t>(phase);
    return idx < names.size() ? names[idx] : "-";
}

struct TopPhase {
    char const* name = "-";
    double avgMs = 0.0;
    double maxMs = 0.0;
};

struct Summary {
    uint32_t windowMs = 0;
    uint32_t frames = 0;
    uint32_t focusedPct = 0;
    double fps = 0.0;
    double frameAvgMs = 0.0, frameMaxMs = 0.0;
    double engineAvgMs = 0.0, engineMaxMs = 0.0;
    double modAvgMs = 0.0, modMaxMs = 0.0;
    double npcsAvgMs = 0.0, npcsMaxMs = 0.0;
    double netAvgMs = 0.0;
    double worldAvgMs = 0.0;
    double otherAvgMs = 0.0, otherMaxMs = 0.0;
    std::array<TopPhase, TopPhaseCount> top{};
};

class Accumulator {
    bool started_ = false;
    uint32_t startedAtMs_ = 0;
    uint32_t frames_ = 0;
    uint32_t intervals_ = 0;
    uint32_t focusedFrames_ = 0;
    uint64_t frameSumUs_ = 0, frameMaxUs_ = 0;
    uint64_t engineSumUs_ = 0, engineMaxUs_ = 0;
    uint64_t modSumUs_ = 0, modMaxUs_ = 0;
    uint64_t npcsSumUs_ = 0, npcsMaxUs_ = 0;
    uint64_t netSumUs_ = 0;
    uint64_t worldSumUs_ = 0;
    uint64_t otherSumUs_ = 0, otherMaxUs_ = 0;
    std::array<uint64_t, SubPhaseCount> subSumUs_{};
    std::array<uint64_t, SubPhaseCount> subMaxUs_{};

    static double ToMs(uint64_t us) noexcept { return static_cast<double>(us) / 1000.0; }
    static double AvgMs(uint64_t sumUs, uint32_t count) noexcept
    {
        return count ? static_cast<double>(sumUs) / (1000.0 * static_cast<double>(count)) : 0.0;
    }

  public:
    void Reset() noexcept { *this = Accumulator{}; }

    void Record(uint32_t nowMs, bool hasInterval, uint64_t frameUs, uint64_t engineUs, uint64_t modUs,
                uint64_t npcsUs, uint64_t netUs, uint64_t worldUs, bool focused,
                uint64_t otherUs = 0, uint64_t const* subUs = nullptr) noexcept
    {
        if (!started_) {
            started_ = true;
            startedAtMs_ = nowMs;
        }
        ++frames_;
        if (focused) ++focusedFrames_;
        if (hasInterval) {
            ++intervals_;
            frameSumUs_ += frameUs;
            frameMaxUs_ = std::max(frameMaxUs_, frameUs);
            engineSumUs_ += engineUs;
            engineMaxUs_ = std::max(engineMaxUs_, engineUs);
        }
        modSumUs_ += modUs;
        modMaxUs_ = std::max(modMaxUs_, modUs);
        npcsSumUs_ += npcsUs;
        npcsMaxUs_ = std::max(npcsMaxUs_, npcsUs);
        netSumUs_ += netUs;
        worldSumUs_ += worldUs;
        otherSumUs_ += otherUs;
        otherMaxUs_ = std::max(otherMaxUs_, otherUs);
        if (subUs) {
            for (std::size_t i = 0; i < SubPhaseCount; ++i) {
                const uint64_t v = subUs[i];
                if (!v) continue;
                subSumUs_[i] += v;
                subMaxUs_[i] = std::max(subMaxUs_[i], v);
            }
        }
    }

    bool Due(uint32_t nowMs, uint32_t periodMs = HeartbeatPeriodMs) const noexcept
    {
        return started_ && frames_ > 0 &&
            static_cast<int32_t>(nowMs - startedAtMs_) >= static_cast<int32_t>(periodMs);
    }

    Summary Drain(uint32_t nowMs) noexcept
    {
        Summary out{};
        if (!started_ || !frames_) return out;
        const int32_t rawElapsed = static_cast<int32_t>(nowMs - startedAtMs_);
        const uint32_t elapsedMs = rawElapsed > 0 ? static_cast<uint32_t>(rawElapsed) : 0u;
        const double spanMs = frameSumUs_ > 0 ? ToMs(frameSumUs_) : static_cast<double>(elapsedMs);
        out.windowMs = elapsedMs;
        out.frames = frames_;
        out.focusedPct = (focusedFrames_ * 100u + frames_ / 2u) / frames_;
        out.fps = spanMs > 0.0
            ? (static_cast<double>(intervals_ ? intervals_ : frames_) * 1000.0 / spanMs)
            : 0.0;
        out.frameAvgMs = AvgMs(frameSumUs_, intervals_);
        out.frameMaxMs = ToMs(frameMaxUs_);
        out.engineAvgMs = AvgMs(engineSumUs_, intervals_);
        out.engineMaxMs = ToMs(engineMaxUs_);
        out.modAvgMs = AvgMs(modSumUs_, frames_);
        out.modMaxMs = ToMs(modMaxUs_);
        out.npcsAvgMs = AvgMs(npcsSumUs_, frames_);
        out.npcsMaxMs = ToMs(npcsMaxUs_);
        out.netAvgMs = AvgMs(netSumUs_, frames_);
        out.worldAvgMs = AvgMs(worldSumUs_, frames_);
        out.otherAvgMs = AvgMs(otherSumUs_, frames_);
        out.otherMaxMs = ToMs(otherMaxUs_);
        std::array<std::size_t, TopPhaseCount> bestIdx{};
        std::array<uint64_t, TopPhaseCount> bestSum{};
        for (std::size_t i = 0; i < SubPhaseCount; ++i) {
            const uint64_t s = subSumUs_[i];
            if (!s) continue;
            for (std::size_t slot = 0; slot < TopPhaseCount; ++slot) {
                if (s > bestSum[slot]) {
                    for (std::size_t shift = TopPhaseCount - 1; shift > slot; --shift) {
                        bestSum[shift] = bestSum[shift - 1];
                        bestIdx[shift] = bestIdx[shift - 1];
                    }
                    bestSum[slot] = s;
                    bestIdx[slot] = i;
                    break;
                }
            }
        }
        for (std::size_t slot = 0; slot < TopPhaseCount; ++slot) {
            if (!bestSum[slot]) break;
            const std::size_t idx = bestIdx[slot];
            out.top[slot] = {
                SubPhaseName(static_cast<SubPhase>(idx)),
                AvgMs(subSumUs_[idx], frames_),
                ToMs(subMaxUs_[idx]),
            };
        }
        Reset();
        started_ = true;
        startedAtMs_ = nowMs;
        return out;
    }
};

inline int FormatFields(char* out, std::size_t size, Summary const& s) noexcept
{
    if (!s.frames || !size) return 0;
    const int head = std::snprintf(
        out, size,
        " fps=%.1f focused_pct=%u frame_ms=%.2f/%.2f engine_ms=%.2f/%.2f mod_ms=%.2f/%.2f "
        "mod_npcs_ms=%.2f/%.2f mod_net_ms=%.2f mod_world_ms=%.2f mod_other_ms=%.2f/%.2f",
        s.fps, s.focusedPct, s.frameAvgMs, s.frameMaxMs, s.engineAvgMs, s.engineMaxMs,
        s.modAvgMs, s.modMaxMs, s.npcsAvgMs, s.npcsMaxMs, s.netAvgMs, s.worldAvgMs,
        s.otherAvgMs, s.otherMaxMs);
    if (head < 0) return head;
    if (static_cast<std::size_t>(head) >= size) return static_cast<int>(size - 1);
    std::size_t pos = static_cast<std::size_t>(head);
    if (s.top[0].avgMs > 0.0) {
        const int n = std::snprintf(out + pos, size - pos, " mod_top=%s:%.2f/%.2f",
                                    s.top[0].name, s.top[0].avgMs, s.top[0].maxMs);
        if (n > 0) pos += std::min<std::size_t>(static_cast<std::size_t>(n), size - pos - 1);
        for (std::size_t i = 1; i < TopPhaseCount && pos + 1 < size; ++i) {
            if (s.top[i].avgMs < 0.005) break;
            const int m = std::snprintf(out + pos, size - pos, ",%s:%.2f/%.2f",
                                        s.top[i].name, s.top[i].avgMs, s.top[i].maxMs);
            if (m > 0) pos += std::min<std::size_t>(static_cast<std::size_t>(m), size - pos - 1);
        }
    } else if (pos + 1 < size) {
        const int n = std::snprintf(out + pos, size - pos, " mod_top=-");
        if (n > 0) pos += std::min<std::size_t>(static_cast<std::size_t>(n), size - pos - 1);
    }
    return static_cast<int>(pos);
}

inline void FormatHeartbeat(char* out, std::size_t size, Summary const& s, unsigned spheres, unsigned processed) noexcept
{
    if (!size) return;
    const int used = std::snprintf(
        out, size, "HEARTBEAT host-perf: window_ms=%u frames=%u spheres=%u processed=%u",
        s.windowMs, s.frames, spheres, processed);
    if (used < 0 || static_cast<std::size_t>(used) >= size) return;
    FormatFields(out + used, size - static_cast<std::size_t>(used), s);
}

class Runtime {
    Accumulator heartbeat_{};
    Accumulator stream_{};
    long long prevEnteredUs_ = 0;
    long long prevExitedUs_ = 0;
    long long enteredUs_ = 0;
    uint64_t worldUs_ = 0;
    uint64_t netUs_ = 0;
    uint64_t npcsUs_ = 0;
    uint64_t otherUs_ = 0;
    std::array<uint64_t, SubPhaseCount> subUs_{};
    uint32_t frameNowMs_ = 0;
    bool focused_ = false;
    bool active_ = false;
    bool prevValid_ = false;

  public:
    void Reset() noexcept { *this = Runtime{}; }

    void BeginFrame(long long enteredUs, uint32_t nowMs, bool focused, bool inWorld) noexcept
    {
        if (!inWorld) {
            active_ = false;
            prevValid_ = false;
            return;
        }
        enteredUs_ = enteredUs;
        frameNowMs_ = nowMs;
        focused_ = focused;
        worldUs_ = netUs_ = npcsUs_ = otherUs_ = 0;
        subUs_.fill(0);
        active_ = true;
    }

    void AddPhase(Phase phase, long long startedUs, long long endedUs) noexcept
    {
        if (!active_ || endedUs <= startedUs) return;
        const uint64_t delta = static_cast<uint64_t>(endedUs - startedUs);
        switch (phase) {
        case Phase::World: worldUs_ += delta; break;
        case Phase::Net: netUs_ += delta; break;
        case Phase::Npcs: npcsUs_ += delta; break;
        }
    }

    void AddSubPhase(SubPhase sub, long long startedUs, long long endedUs) noexcept
    {
        const std::size_t idx = static_cast<std::size_t>(sub);
        if (!active_ || endedUs <= startedUs || idx >= SubPhaseCount) return;
        const uint64_t delta = static_cast<uint64_t>(endedUs - startedUs);
        otherUs_ += delta;
        subUs_[idx] += delta;
    }

    void EndFrame(long long exitedUs) noexcept
    {
        if (!active_) return;
        const uint64_t modUs = exitedUs > enteredUs_ ? static_cast<uint64_t>(exitedUs - enteredUs_) : 0u;
        const bool hasInterval = prevValid_ && enteredUs_ > prevEnteredUs_ && enteredUs_ >= prevExitedUs_;
        const uint64_t frameUs = hasInterval ? static_cast<uint64_t>(enteredUs_ - prevEnteredUs_) : 0u;
        const uint64_t engineUs = hasInterval ? static_cast<uint64_t>(enteredUs_ - prevExitedUs_) : 0u;
        heartbeat_.Record(frameNowMs_, hasInterval, frameUs, engineUs, modUs, npcsUs_, netUs_, worldUs_, focused_,
                          otherUs_, subUs_.data());
        stream_.Record(frameNowMs_, hasInterval, frameUs, engineUs, modUs, npcsUs_, netUs_, worldUs_, focused_,
                       otherUs_, subUs_.data());
        prevEnteredUs_ = enteredUs_;
        prevExitedUs_ = exitedUs;
        prevValid_ = true;
        active_ = false;
    }

    bool PollHeartbeat(uint32_t nowMs, Summary& out) noexcept
    {
        if (!heartbeat_.Due(nowMs, HeartbeatPeriodMs)) return false;
        out = heartbeat_.Drain(nowMs);
        return out.frames > 0;
    }

    Summary TakeStreamSummary(uint32_t nowMs) noexcept
    {
        return stream_.Drain(nowMs);
    }
};

inline Runtime& Global() noexcept
{
    static Runtime runtime;
    return runtime;
}

} // namespace g3t::host_frame_diag
