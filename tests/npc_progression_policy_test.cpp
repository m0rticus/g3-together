// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/npc_progression_policy.h"

#include <cassert>
#include <cstring>

namespace {

void TestDialogAndDefeatedBitPacking()
{
    using namespace g3t::npc_progression_policy;

    // Preserve Teach/Trade/Party/Mob/Slavery flags + DialogPresent while packing TalkedToPlayer in 0x20.
    const uint8_t packedTrue = PackDialog(true, false, true, false, true, true);
    assert(HasDialog(packedTrue));
    assert((packedTrue & (DialogTeach | DialogTrade | DialogParty | DialogMob | DialogSlavery)) ==
           (DialogTeach | DialogParty | DialogSlavery));
    assert(UnpackTalkedToPlayer(packedTrue));

    const uint8_t packedFalse = PackDialog(true, true, true, true, true, false);
    assert(HasDialog(packedFalse));
    assert(!UnpackTalkedToPlayer(packedFalse));
    assert(!HasDialog(0u));
    assert(!UnpackTalkedToPlayer(0u));

    // Legacy payloads without NpcHasDefeated leave DefeatedByPlayer untouched; explicit payloads distinguish false vs true.
    assert(!HasNpcFlags(0u));
    assert(!HasDefeatedFlag(0u));
    assert(HasNpcFlags(NpcPresent));
    assert(!HasDefeatedFlag(NpcPresent));
    assert(!UnpackDefeatedByPlayer(NpcPresent));

    const uint8_t explicitFalse = PackNpcFlags(true, false);
    assert(HasNpcFlags(explicitFalse));
    assert(HasDefeatedFlag(explicitFalse));
    assert(!UnpackDefeatedByPlayer(explicitFalse));

    const uint8_t explicitTrue = PackNpcFlags(true, true);
    assert(HasNpcFlags(explicitTrue));
    assert(HasDefeatedFlag(explicitTrue));
    assert(UnpackDefeatedByPlayer(explicitTrue));
}

void TestMonotonicHostWorldStateMerge()
{
    using namespace g3t::npc_progression_policy;

    // A stale snapshot arriving with TalkedToPlayer=false or DefeatedByPlayer=false updates mutable party/trade flags
    // without erasing monotonic TalkedToPlayer / DefeatedByPlayer progression on the host.
    const uint8_t incomingDialog = PackDialog(false, false, false, true, false, false);
    const uint8_t mergedDialog = MergeHostDialog(incomingDialog, true);
    assert((mergedDialog & DialogMob) != 0);
    assert(UnpackTalkedToPlayer(mergedDialog));

    const uint8_t incomingNpc = PackNpcFlags(true, false);
    const uint8_t mergedNpc = MergeHostNpcFlags(incomingNpc, true);
    assert(HasDefeatedFlag(mergedNpc));
    assert(UnpackDefeatedByPlayer(mergedNpc));

    // Fresh false -> true advancement sticks cleanly, and absent headers stay absent.
    assert(UnpackTalkedToPlayer(MergeHostDialog(PackDialog(true, false, false, false, false, true), false)));
    assert(UnpackDefeatedByPlayer(MergeHostNpcFlags(PackNpcFlags(true, true), false)));
    assert(MergeHostDialog(0u, true) == 0u);
    assert(MergeHostNpcFlags(0u, true) == 0u);
}

void TestFriendMemoryMappingAndLocalProgressionDetection()
{
    using namespace g3t::npc_progression_policy;

    // Lester -> Diego/Gorn Ardea handoff infos gate on PlayerMemory.TalkedToLester / TalkedToDiego / TalkedToGorn / TalkedToMilten.
    assert(FriendMemoryForName("Lester") == FriendMemoryFlag::Lester);
    assert(FriendMemoryForName("Diego") == FriendMemoryFlag::Diego);
    assert(FriendMemoryForName("Gorn") == FriendMemoryFlag::Gorn);
    assert(FriendMemoryForName("Milten") == FriendMemoryFlag::Milten);
    assert(FriendMemoryForName("Hamlar") == FriendMemoryFlag::None);
    assert(FriendMemoryForName("Diego_Copy") == FriendMemoryFlag::None);
    assert(FriendMemoryForName("") == FriendMemoryFlag::None);
    assert(FriendMemoryForName(nullptr) == FriendMemoryFlag::None);

    ObservedProgression base{PackDialog(true, true, false, false, false, false), 1, 2, 3, 0, PackNpcFlags(true, false)};
    assert(base == base);

    // EndInfoManager flipping Dialog.TalkedToPlayer = true without an InfoCommand changes ObservedProgression.
    ObservedProgression talked = base;
    talked.dialog = PackDialog(true, true, false, false, false, true);
    assert(base != talked);

    // Knockout flipping DefeatedByPlayer = true changes ObservedProgression.
    ObservedProgression defeated = base;
    defeated.npc = PackNpcFlags(true, true);
    assert(base != defeated);

    // Seed pre-existing cross-town progression on host for talked/defeated NPCs, and gate authorship cleanly.
    assert(IsNonDefaultSeedCandidate(true, false));
    assert(IsNonDefaultSeedCandidate(false, true));
    assert(!IsNonDefaultSeedCandidate(false, false));

    assert(ShouldAuthorProgressionDiff(true, false, false, false, false, false));
    assert(!ShouldAuthorProgressionDiff(true, true, false, false, false, false));
    assert(!ShouldAuthorProgressionDiff(true, false, true, false, false, false));
    assert(!ShouldAuthorProgressionDiff(true, false, false, true, false, false));
    assert(ShouldAuthorProgressionDiff(false, false, false, false, true, false));
    assert(ShouldAuthorProgressionDiff(false, false, false, false, false, true));
    assert(!ShouldAuthorProgressionDiff(false, false, false, false, false, false));
}

} // namespace

int main()
{
    TestDialogAndDefeatedBitPacking();
    TestMonotonicHostWorldStateMerge();
    TestFriendMemoryMappingAndLocalProgressionDetection();
    return 0;
}
