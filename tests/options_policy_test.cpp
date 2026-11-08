// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/options_policy.h"

#include <cassert>
#include <limits>

using namespace g3t::options::policy;
struct Setup {
    bool AlternativeBalancing = false, AlternativeAI = false;
    int MinHitDuration = 6, XPModifier = 100;
    std::string Difficulty = "Easy";
    bool QuickLoot = false;
    int Width = 1920, sound = 75;
};
struct Config {
    int friendlyFire = 0;
    float npcDamage = 1.0f, npcHp = 1.0f;
    bool clientAutoload = false;
};

void Alter(Snapshot& s, unsigned bit)
{
    switch (bit) {
    case Balancing: s.balancing = 1; break;
    case AI: s.ai = 1; break;
    case MinHit: s.minHit = 15; break;
    case XP: s.xp = 150; break;
    case FriendlyFire: s.friendlyFire = 50; break;
    case NpcDamage: s.npcDamage = 1.234567f; break;
    case NpcHp: s.npcHp = 9.999f; break;
    case PlayerStart: std::strcpy(s.playerStart, "Diego"); break;
    default: assert(false);
    }
}
int main()
{
    for (auto* key : {"difficulty", "quickloot", "quick_loot", "testmode", "nomenu"})
        assert(Classify(key) == Class::Local);
    for (auto* key : {"alternative_balancing", "extendedcontent", "alternative_ai", "aimode",
                     "minhitduration", "min_hit_duration", "xpmodifier", "xp_modifier",
                     "friendly_fire", "npc_damage", "npc_hp", "playerstart"})
        assert(Classify(key) == Class::Host);
    for (auto* key : {"Game.dll", "Script.dll", "Engine.dll", "scripts folder",
                     "rules archives", "other archives", "protocol", "UnknownGameplay"})
        assert(Classify(key) == Class::Identity);
    assert(Classify("  ExTendedContent\t") == Class::Host);
    assert(LowerTrim(" Value \r\n") == "value");
    Snapshot local;
    assert(Valid(local));
    for (unsigned bit = 1; bit <= PlayerStart; bit <<= 1) {
        for (bool mismatch : {false, true}) {
            Session session;
            auto host = local;
            if (mismatch) Alter(host, bit);
            assert(session.Adopt(local, host, 1));
            assert(Differences(session.Effective(local), host) == 0);
            assert(Differences(session.original, local) == 0);
            assert(Differences(local, host) == (mismatch ? bit : 0));
            // Duplicate announcement, then a new epoch's opposite host setting.
            assert(session.Adopt(local, host, 1));
            auto next = local;
            if (!mismatch) Alter(next, bit);
            assert(!session.Adopt(local, next, 1)); // Changes require a new host epoch.
            assert(session.Adopt(local, next, 2));
            assert(Differences(session.host, next) == 0);
            assert(Differences(session.original, local) == 0);
            auto invalid = next; invalid.xp = 0;
            assert(!session.Adopt(local, invalid, 3));
            assert(Differences(session.host, next) == 0);
            session.Restore();
            assert(!session.active);
            assert(Differences(session.Effective(local), local) == 0);
            assert(session.Adopt(next, host)); // new session captures a new original
            assert(Differences(session.original, next) == 0);
            assert(NeedsEntry(bit, true) == (bit == Balancing || bit == PlayerStart));
            assert(!NeedsEntry(bit, false));
        }
    }
    auto host = local;
    for (unsigned bit = 1; bit <= PlayerStart; bit <<= 1) Alter(host, bit);
    std::strcpy(host.difficulty, "Hard"); host.quickLoot = 1;
    Setup setup; const auto savedSetup = setup;
    ApplySetup(setup, host);
    assert(setup.AlternativeBalancing && setup.AlternativeAI);
    assert(setup.MinHitDuration == 15 && setup.XPModifier == 150);
    assert(setup.Difficulty == "Easy" && !setup.QuickLoot);
    assert(setup.Width == 1920 && setup.sound == 75);
    assert(savedSetup.XPModifier == 100 && savedSetup.Difficulty == "Easy");
    Config config; const auto savedConfig = config;
    ApplyMod(config, host);
    assert(config.friendlyFire == host.friendlyFire && config.npcDamage == host.npcDamage && config.npcHp == host.npcHp);
    assert(!config.clientAutoload && savedConfig.npcDamage == 1.0f);
    auto kept = host; std::strcpy(kept.difficulty, "Easy"); kept.quickLoot = 0;
    assert(Differences(host, kept) == 0); // no notice for local choices
    assert(IsSetupSerializer(0x90b0e) && !IsSetupSerializer(0x15920c));
    for (int value : {-1, 16, 32767}) {
        auto bad = local; bad.minHit = static_cast<int16_t>(value); assert(!Valid(bad));
    }
    for (int value : {0, 201}) { auto bad = local; bad.xp = value; assert(!Valid(bad)); }
    for (int value : {-1, 101}) { auto bad = local; bad.friendlyFire = value; assert(!Valid(bad)); }
    for (float value : {0.0f, 10.01f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        auto bad = local; bad.npcDamage = value; assert(!Valid(bad));
        bad = local; bad.npcHp = value; assert(!Valid(bad));
    }
    for (auto* value : {"", "..\\save", "two words"}) {
        auto bad = local; std::strcpy(bad.playerStart, value); assert(!Valid(bad));
    }
    auto bad = local; std::memset(bad.playerStart, 'x', sizeof(bad.playerStart)); assert(!Valid(bad));
    bad = local; bad.schema = 0; assert(!Valid(bad));
    bad = local; bad.balancing = 2; assert(!Valid(bad));
    bad = local; bad.ai = 2; assert(!Valid(bad));
    bad = local; bad.quickLoot = 2; assert(!Valid(bad));
    bad = local; std::memset(bad.difficulty, 'x', sizeof(bad.difficulty)); assert(!Valid(bad));
}
