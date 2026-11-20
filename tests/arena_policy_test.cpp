// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/arena_policy.h"

#include <cassert>

namespace {

void TestQuestTypeAndBoutStartTransitions()
{
    using namespace g3t::arena_policy;

    assert(IsArenaOrSparringQuestType(QuestTypeArena));
    assert(IsArenaOrSparringQuestType(QuestTypeSparring));
    assert(!IsArenaOrSparringQuestType(0));
    assert(!IsArenaOrSparringQuestType(3));

    // Fresh bouts (Open -> Running) and Reactivator rematches (Cancelled/Lost -> Running) are bout starts.
    assert(IsBoutStartTransition(QuestTypeArena, QuestStatusOpen, QuestStatusRunning));
    assert(IsBoutStartTransition(QuestTypeArena, QuestStatusCancelled, QuestStatusRunning));
    assert(IsBoutStartTransition(QuestTypeArena, QuestStatusLost, QuestStatusRunning));
    assert(IsCandidateBoutQuestStatus(QuestStatusOpen));
    assert(IsCandidateBoutQuestStatus(QuestStatusLost));
    assert(IsCandidateBoutQuestStatus(QuestStatusCancelled));
    assert(!IsCandidateBoutQuestStatus(QuestStatusRunning));
    assert(!IsCandidateBoutQuestStatus(QuestStatusSuccess));
    assert(!IsCandidateBoutQuestStatus(QuestStatusWon));
    assert(IsBoutStartTransition(QuestTypeSparring, QuestStatusOpen, QuestStatusRunning));
    assert(IsBoutStartTransition(QuestTypeSparring, QuestStatusLost, QuestStatusRunning));
    assert(!IsBoutStartTransition(QuestTypeArena, QuestStatusRunning, QuestStatusRunning));
    assert(!IsBoutStartTransition(QuestTypeArena, QuestStatusRunning, QuestStatusSuccess));
    assert(!IsBoutStartTransition(0, QuestStatusOpen, QuestStatusRunning));

    // Client claims locally started Arena/Sparring quests, but never claims Running states received from host sync.
    assert(ShouldClaimLocalClientBout(false, true, QuestTypeArena, QuestStatusOpen, QuestStatusRunning, false));
    assert(ShouldClaimLocalClientBout(false, true, QuestTypeArena, QuestStatusLost, QuestStatusRunning, false));
    assert(ShouldClaimLocalClientBout(false, true, QuestTypeSparring, QuestStatusOpen, QuestStatusRunning, false));
    assert(!ShouldClaimLocalClientBout(false, true, QuestTypeArena, QuestStatusOpen, QuestStatusRunning, true));
    assert(!ShouldClaimLocalClientBout(true, true, QuestTypeArena, QuestStatusOpen, QuestStatusRunning, false));
    assert(!ShouldClaimLocalClientBout(false, false, QuestTypeArena, QuestStatusOpen, QuestStatusRunning, false));
}

void TestArenaStatusComputation()
{
    using namespace g3t::arena_policy;

    assert(DesiredArenaStatus(true) == ArenaStatusRunning);
    assert(DesiredArenaStatus(false) == ArenaStatusNone);
}

void TestHostClientQuestStatusFiltering()
{
    using namespace g3t::arena_policy;

    // Non-arena quests always pass through unchanged.
    assert(ShouldAcceptClientQuestStatusOnHost(0, QuestStatusRunning, 1, 2, QuestStatusCancelled));

    // Client can start Open/Lost/Cancelled -> Running bouts and rematches.
    assert(ShouldAcceptClientQuestStatusOnHost(QuestTypeArena, QuestStatusOpen, 0, 2, QuestStatusRunning));
    assert(ShouldAcceptClientQuestStatusOnHost(QuestTypeArena, QuestStatusLost, 0, 2, QuestStatusRunning));
    assert(ShouldAcceptClientQuestStatusOnHost(QuestTypeArena, QuestStatusCancelled, 0, 2, QuestStatusRunning));
    assert(ShouldAcceptClientQuestStatusOnHost(QuestTypeSparring, QuestStatusOpen, 0, 2, QuestStatusRunning));

    // Stale Open deltas never revert Running, Lost, Cancelled, Success, or Won states.
    assert(!ShouldAcceptClientQuestStatusOnHost(QuestTypeArena, QuestStatusRunning, 2, 2, QuestStatusOpen));
    assert(!ShouldAcceptClientQuestStatusOnHost(QuestTypeArena, QuestStatusLost, 0, 2, QuestStatusOpen));
    assert(!ShouldAcceptClientQuestStatusOnHost(QuestTypeArena, QuestStatusCancelled, 0, 2, QuestStatusOpen));
    assert(!ShouldAcceptClientQuestStatusOnHost(QuestTypeArena, QuestStatusSuccess, 0, 2, QuestStatusOpen));
    assert(!ShouldAcceptClientQuestStatusOnHost(QuestTypeArena, QuestStatusWon, 0, 2, QuestStatusOpen));

    // Spectator client (Player 3 or Player 2 during Host Player 1's bout) cannot cancel or lose another player's Running bout.
    assert(!ShouldAcceptClientQuestStatusOnHost(QuestTypeArena, QuestStatusRunning, 1, 2, QuestStatusLost));
    assert(!ShouldAcceptClientQuestStatusOnHost(QuestTypeArena, QuestStatusRunning, 1, 2, QuestStatusCancelled));
    assert(!ShouldAcceptClientQuestStatusOnHost(QuestTypeArena, QuestStatusRunning, 2, 3, QuestStatusLost));
    assert(!ShouldAcceptClientQuestStatusOnHost(QuestTypeArena, QuestStatusRunning, 2, 3, QuestStatusCancelled));

    // Active client challenger (Player 2) CAN legitimately forfeit/cancel/win their own Running bout.
    assert(ShouldAcceptClientQuestStatusOnHost(QuestTypeArena, QuestStatusRunning, 2, 2, QuestStatusLost));
    assert(ShouldAcceptClientQuestStatusOnHost(QuestTypeArena, QuestStatusRunning, 2, 2, QuestStatusCancelled));
    assert(ShouldAcceptClientQuestStatusOnHost(QuestTypeArena, QuestStatusRunning, 2, 2, QuestStatusSuccess));
    assert(ShouldAcceptClientQuestStatusOnHost(QuestTypeArena, QuestStatusRunning, 2, 2, QuestStatusWon));

    // Completed Success/Won quests cannot be regressed to Lost or Cancelled by late client deltas.
    assert(!ShouldAcceptClientQuestStatusOnHost(QuestTypeArena, QuestStatusSuccess, 2, 2, QuestStatusLost));
    assert(!ShouldAcceptClientQuestStatusOnHost(QuestTypeArena, QuestStatusSuccess, 2, 2, QuestStatusRunning));
    assert(ShouldAcceptClientQuestStatusOnHost(QuestTypeArena, QuestStatusSuccess, 2, 2, QuestStatusSuccess));
}

void TestSpectatorRingExitSuppression()
{
    using namespace g3t::arena_policy;

    // Host hero (1) walking out of Reddock_Arena while Client 2 is fighting must NOT cancel Client 2's bout.
    assert(ShouldSuppressPlayerLeaveArea(true, true, 2, 1));
    // Client 2 walking out of Reddock_Arena while Host 1 is fighting must NOT cancel Host 1's bout.
    assert(ShouldSuppressPlayerLeaveArea(true, true, 1, 2));
    // Client 3 walking out while Client 2 is fighting must NOT cancel Client 2's bout.
    assert(ShouldSuppressPlayerLeaveArea(true, true, 2, 3));
    // Client 2 walking out when there is an active bout synced from host (challenger=0 locally on client) suppresses spectator interference.
    assert(ShouldSuppressPlayerLeaveArea(true, true, 0, 2));
    // Host 1 walking out before ObserveQuestTransitions populates challenger=1 still cancels Host 1's own bout.
    assert(!ShouldSuppressPlayerLeaveArea(true, true, 0, 1));

    // Active challenger walking out of their OWN bout MUST NOT be suppressed (normal ring-exit cancellation applies!).
    assert(!ShouldSuppressPlayerLeaveArea(true, true, 1, 1));
    assert(!ShouldSuppressPlayerLeaveArea(true, true, 2, 2));
    // Non-arena or inactive arena zones are never suppressed.
    assert(!ShouldSuppressPlayerLeaveArea(true, false, 2, 1));
    assert(!ShouldSuppressPlayerLeaveArea(false, true, 2, 1));
}

void TestFighterTaskAndDefeatPolicies()
{
    using namespace g3t::arena_policy;

    // Fighter busy-task classification across walk, combat, stumbles, KO, ransack, and standing up.
    assert(IsArenaFighterBusyTask("ZS_Arena"));
    assert(IsArenaFighterBusyTask("ZS_Attack"));
    assert(IsArenaFighterBusyTask("ZS_Attack_Arena_End"));
    assert(IsArenaFighterBusyTask("ZS_Unconscious"));
    assert(IsArenaFighterBusyTask("ZS_RansackBody"));
    assert(IsArenaFighterBusyTask("ZS_StandUp"));
    assert(!IsArenaFighterBusyTask("ZS_Talk"));
    assert(!IsArenaFighterBusyTask("ZS_Stand"));
    assert(!IsArenaFighterBusyTask("ZS_SitBench"));
    assert(!IsArenaFighterBusyTask(nullptr));

    assert(HasFighterEnteredBoutTask("ZS_Arena"));
    assert(HasFighterEnteredBoutTask("ZS_Attack"));
    assert(HasFighterEnteredBoutTask("ZS_Attack_Wait"));
    assert(!HasFighterEnteredBoutTask("ZS_Attack_End"));
    assert(!HasFighterEnteredBoutTask("ZS_Attack_Arena_End"));
    assert(!HasFighterEnteredBoutTask("ZS_Talk"));
    assert(!HasFighterEnteredBoutTask("ZS_Stand"));
    assert(!HasFighterEnteredBoutTask(nullptr));

    // SendToArena host dispatch waits for quest Running, talk hold release, lease fence release, and ZS_Talk exit,
    // while allowing rematch dispatch if the fighter is still in ZS_Attack_End / ZS_Attack_Arena_End.
    assert(ShouldDispatchSendToArenaOnHost(true, false, false, "ZS_Stand"));
    assert(ShouldDispatchSendToArenaOnHost(true, false, false, "ZS_Attack_End"));
    assert(ShouldDispatchSendToArenaOnHost(true, false, false, "ZS_Attack_Arena_End"));
    assert(!ShouldDispatchSendToArenaOnHost(false, false, false, "ZS_Stand"));
    assert(!ShouldDispatchSendToArenaOnHost(true, true, false, "ZS_Stand"));
    assert(!ShouldDispatchSendToArenaOnHost(true, false, true, "ZS_Stand"));
    assert(!ShouldDispatchSendToArenaOnHost(true, false, false, "ZS_Talk"));
    assert(!ShouldDispatchSendToArenaOnHost(true, false, false, "ZS_Arena"));
    assert(!ShouldDispatchSendToArenaOnHost(true, false, false, "ZS_Attack"));

    // Client suppresses OnLeaveArea/OnEnterArea for passive mirrored NPCs and remote puppets.
    assert(ShouldSuppressNonLocalAreaActor(false, true, false, false));
    assert(!ShouldSuppressNonLocalAreaActor(false, true, true, false));
    assert(!ShouldSuppressNonLocalAreaActor(false, true, false, true));
    assert(!ShouldSuppressNonLocalAreaActor(true, true, false, false));
    assert(!ShouldSuppressNonLocalAreaActor(false, false, false, false));

    // Client challenger defeat/absence resolution on host.
    assert(ShouldMarkClientChallengerDefeatedOnHost(true, 2, true, true, true, false, false));
    assert(!ShouldMarkClientChallengerDefeatedOnHost(true, 1, true, true, true, false, false));  // host hero uses native fightmem
    assert(!ShouldMarkClientChallengerDefeatedOnHost(true, 2, false, true, true, false, false)); // bout not started yet
    assert(!ShouldMarkClientChallengerDefeatedOnHost(true, 2, true, false, true, false, false)); // quest not Running
    assert(!ShouldMarkClientChallengerDefeatedOnHost(true, 2, true, true, false, false, false)); // challenger still upright
    assert(!ShouldMarkClientChallengerDefeatedOnHost(true, 2, true, true, true, true, false));   // fighter dead -> victory wins
    assert(!ShouldMarkClientChallengerDefeatedOnHost(true, 2, true, true, true, false, true));   // fighter down -> victory wins

    assert(ShouldForfeitAbsentChallengerOnHost(true, 2, true, false));
    assert(!ShouldForfeitAbsentChallengerOnHost(true, 2, true, true));
    assert(!ShouldForfeitAbsentChallengerOnHost(true, 1, true, false));
    assert(!ShouldForfeitAbsentChallengerOnHost(false, 2, true, false));
}

void TestClientDialogAttackForwarding()
{
    using namespace g3t::arena_policy;

    // Both mirrored NPCs AND client-leased NPCs must forward dialog Attack against local player to host!
    assert(ShouldForwardClientDialogAttack(false, true, true, true, false, true));   // mirrored NPC
    assert(ShouldForwardClientDialogAttack(false, true, true, false, true, true));   // client-leased NPC (bug in Build 83!)
    assert(!ShouldForwardClientDialogAttack(true, true, true, false, true, true));   // host executes locally
    assert(!ShouldForwardClientDialogAttack(false, false, true, true, false, true)); // disconnected
    assert(!ShouldForwardClientDialogAttack(false, true, false, true, false, true)); // missing target
    assert(!ShouldForwardClientDialogAttack(false, true, true, true, false, false)); // target is not local player
    assert(!ShouldForwardClientDialogAttack(false, true, true, false, false, true)); // neither mirrored nor leased
}

} // namespace

int main()
{
    TestQuestTypeAndBoutStartTransitions();
    TestArenaStatusComputation();
    TestHostClientQuestStatusFiltering();
    TestSpectatorRingExitSuppression();
    TestFighterTaskAndDefeatPolicies();
    TestClientDialogAttackForwarding();
    return 0;
}
