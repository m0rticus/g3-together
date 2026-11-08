// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace g3t::options::policy {
enum class Class { Local, Host, Identity };

inline std::string LowerTrim(std::string text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    for (char& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return text;
}

inline Class Classify(std::string key)
{
    key = LowerTrim(key);
    if (key == "difficulty" || key == "quickloot" || key == "quick_loot" ||
        key == "testmode" || key == "nomenu") return Class::Local;
    if (key == "alternative_balancing" || key == "extendedcontent" || key == "alternative_ai" ||
        key == "aimode" || key == "minhitduration" || key == "min_hit_duration" ||
        key == "xpmodifier" || key == "xp_modifier" || key == "friendly_fire" ||
        key == "npc_damage" || key == "npc_hp" || key == "playerstart") return Class::Host;
    return Class::Identity;
}

// A capability marker in Hello's existing Options slot rejects pre-delegation builds.
constexpr uint32_t Schema = 0x314f3347; // G3O1
constexpr uint64_t Capability = 0x314f33470000004dull;
#pragma pack(push, 1)
struct Snapshot {
    uint32_t schema = Schema;
    uint8_t balancing = 0, ai = 0;
    int16_t minHit = 6;
    int32_t xp = 100, friendlyFire = 0;
    float npcDamage = 1.0f, npcHp = 1.0f;
    char playerStart[64] = "PC_Hero";
    char difficulty[16] = "Medium"; // Informational / NPC judgement owner, never the client's hero setting.
    uint8_t quickLoot = 1; // Informational; local input is kept.
};
#pragma pack(pop)
static_assert(sizeof(Snapshot) == 105, "balancing snapshot wire layout");

inline bool Valid(Snapshot const& s)
{
    if (s.schema != Schema || s.balancing > 1 || s.ai > 1 || s.minHit < 0 || s.minHit > 15 ||
        s.xp < 1 || s.xp > 200 || s.friendlyFire < 0 || s.friendlyFire > 100 ||
        !std::isfinite(s.npcDamage) || s.npcDamage < 0.1f || s.npcDamage > 10.0f ||
        !std::isfinite(s.npcHp) || s.npcHp < 0.1f || s.npcHp > 10.0f ||
        !s.playerStart[0] || !std::memchr(s.playerStart, 0, sizeof(s.playerStart)) || s.quickLoot > 1 ||
        (!std::memchr(s.difficulty, 0, sizeof(s.difficulty))) ||
        (std::strcmp(s.difficulty, "Easy") && std::strcmp(s.difficulty, "Medium") &&
         std::strcmp(s.difficulty, "Hard"))) return false;
    for (char const* c = s.playerStart; *c; ++c)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || *c == '_' || *c == '-' || *c == '.')) return false;
    return true;
}

enum : unsigned { Balancing = 1, AI = 2, MinHit = 4, XP = 8, FriendlyFire = 16,
                  NpcDamage = 32, NpcHp = 64, PlayerStart = 128 };
inline unsigned Differences(Snapshot const& a, Snapshot const& b)
{
    return (a.balancing != b.balancing ? Balancing : 0) | (a.ai != b.ai ? AI : 0) |
           (a.minHit != b.minHit ? MinHit : 0) | (a.xp != b.xp ? XP : 0) |
           (a.friendlyFire != b.friendlyFire ? FriendlyFire : 0) |
           (a.npcDamage != b.npcDamage ? NpcDamage : 0) | (a.npcHp != b.npcHp ? NpcHp : 0) |
           (std::strcmp(a.playerStart, b.playerStart) ? PlayerStart : 0);
}

struct Session {
    bool active = false;
    uint32_t epoch = 0;
    Snapshot original{}, host{};
    bool Adopt(Snapshot const& local, Snapshot const& received, uint32_t world = 0)
    {
        if (!Valid(received) || (active && world && epoch == world && Differences(host, received))) return false;
        if (!active) original = local;
        host = received;
        epoch = world;
        active = true;
        return true;
    }
    Snapshot Effective(Snapshot const& local) const { return active ? host : local; }
    void Restore() { *this = {}; }
};

inline bool IsSetupSerializer(uintptr_t returnRva) { return returnRva == 0x90b0e; }
inline bool NeedsEntry(unsigned changes, bool running) { return running && (changes & (Balancing | PlayerStart)); }

template<class Setup> void ApplySetup(Setup& effective, Snapshot const& host)
{
    effective.AlternativeBalancing = host.balancing != 0;
    effective.AlternativeAI = host.ai != 0;
    effective.MinHitDuration = host.minHit;
    effective.XPModifier = host.xp;
}
template<class Config> void ApplyMod(Config& effective, Snapshot const& host)
{
    effective.friendlyFire = host.friendlyFire;
    effective.npcDamage = host.npcDamage;
    effective.npcHp = host.npcHp;
}
}
