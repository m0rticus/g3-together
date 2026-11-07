// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/npcremoval_policy.h"

#include <cassert>
#include <limits>
#include <string>

using namespace g3t::npcremoval::policy;

void Names()
{
    uint8_t names = Defaults;
    assert(ParseNames(" Goblin , Ogre ", names) && names == Defaults);
    assert(ParseNames("Ogre,Goblin", names) && names == Defaults);
    assert(ParseNames("Goblin", names) && names == Goblin);
    assert(ParseNames("Ogre", names) && names == Ogre);
    assert(ParseNames(" \t", names) && names == 0);
    for (auto invalid : {"Goblin,", ",Ogre", "Goblin,,Ogre", "Goblin,Goblin", "Ogre,Ogre",
                         "goblin", "OGRE", "GoblinShaman", "Ardea_Goblin", "Goblin;Ogre", "*", "off"}) {
        names = Defaults;
        assert(!ParseNames(invalid, names) && names == Defaults);
    }
    names = Ogre;
    assert(!ParseNames(std::string(512, 'G'), names) && names == Ogre);
}

void Scope()
{
    assert(Count == 24);
    constexpr auto logged = Id("18FB0A54-8C5C-4CF3-8323-08B4EB771152");
    static_assert(logged[0] == 0x54 && logged[1] == 0x0a && logged[4] == 0x5c &&
                  logged[6] == 0xf3 && logged[8] == 0x83 && logged[15] == 0x52, "native GUID byte order");
    const float authored[3] = {92341.69f, -468.47f, -11270.38f};
    const float loaded[3] = {91070.5f, 5020.2f, -12664};
    assert(Matches(Defaults, true, logged.data(), "Goblin", authored, false));
    assert(Matches(Defaults, true, logged.data(), "Goblin", loaded, false));
    assert(!Matches(Defaults, false, logged.data(), "Goblin", loaded, false));
    assert(!Matches(Defaults, true, logged.data(), "Goblin", loaded, true));
    assert(!Matches(0, true, logged.data(), "Goblin", loaded, false));
    assert(!Matches(Ogre, true, logged.data(), "Goblin", loaded, false));
    for (auto name : {"Slave_08", "Orc", "GoblinShaman", "BlackGoblin", "Trelis_Ogre_01", "Ogre_Gobbo",
                      "Reddock_Goblin_01", "Tower_Goblin", "Faring_Goblin_01", "Montera_Goblin_01"})
        assert(!Matches(Defaults, true, logged.data(), name, loaded, false));
    auto unknown = logged;
    unknown[15] ^= 1;
    assert(!Matches(Defaults, true, unknown.data(), "Goblin", loaded, false));
    assert(Index(nullptr) == -1);
    unsigned goblins = 0, ogres = 0;
    for (size_t i = 0; i != Count; ++i) {
        assert(Index(Entries[i].guid.data()) == static_cast<int>(i));
        assert(Matches(Defaults, true, Entries[i].guid.data(), Entries[i].name, authored, false));
        for (size_t j = i + 1; j != Count; ++j)
            assert(Entries[i].guid != Entries[j].guid);
        if (Entries[i].bit == Goblin) ++goblins;
        else if (Entries[i].bit == Ogre) ++ogres;
        else assert(false);
    }
    assert(goblins == 22 && ogres == 2);
    const float limits[2][3] = {{85000, -1500, -20000}, {100000, 9000, -5000}};
    assert(InRegion(limits[0]) && InRegion(limits[1]));
    for (int axis = 0; axis != 3; ++axis) {
        for (int side = 0; side != 2; ++side) {
            float outside[3] = {loaded[0], loaded[1], loaded[2]};
            outside[axis] = limits[side][axis] + (side ? 1.0f : -1.0f);
            assert(!Matches(Defaults, true, logged.data(), "Goblin", outside, false));
        }
        for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
            float bad[3] = {loaded[0], loaded[1], loaded[2]};
            bad[axis] = invalid;
            assert(!InRegion(bad));
        }
    }
    assert(!InRegion(nullptr));
}

void Lifecycle()
{
    Fence fence;
    assert(!fence.Removed());
    assert(!fence.Accept(0, 0, 1, 0));
    assert(!fence.Accept(7, 8, 1, 0));
    assert(!fence.Accept(8, 8, 0, 0));
    assert(!fence.Accept(8, 8, 1, 2));
    assert(fence.Accept(8, 8, 2, 2) && fence.Removed());
    assert(!fence.Accept(8, 8, 1, 0) && fence.incarnation == 2);
    assert(fence.Accept(8, 8, 2, 0));
    assert(fence.Accept(8, 8, 3, 0) && fence.Removed());
    // A removal is terminal for this reviewed identity, even if a later snapshot claims a new incarnation.
    assert(fence.Removed());
    fence = Fence{};
    assert(!fence.Removed());

    Publication sent;
    assert(sent.Needs(8, 1, 4));
    // Failed queueing leaves the baseline pending.
    assert(sent.Needs(8, 1, 4));
    sent.Confirm(8, 1, 4);
    assert(!sent.Needs(8, 1, 4));
    assert(sent.Needs(8, 2, 4)); // same peer slot, new connection
    assert(sent.Needs(8, 1, 5)); // same connection and epoch, client reload/re-entry
    assert(sent.Needs(9, 1, 4)); // host world reload
    sent = Publication{};
    assert(sent.Needs(8, 1, 4)); // world gate closes and opens again

    assert(StopTargetInstruction(false, false, true, false, false));
    assert(StopTargetInstruction(false, false, false, true, false));
    assert(StopTargetInstruction(false, false, false, false, true));
    assert(!StopTargetInstruction(true, false, true, true, true));
    assert(!StopTargetInstruction(false, true, true, true, true));
    assert(!StopTargetInstruction(false, false, false, false, false));
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    const std::string section = argv[1];
    if (section == "names") Names();
    else if (section == "scope") Scope();
    else if (section == "lifecycle") Lifecycle();
    else assert(false);
}
