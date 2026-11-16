// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/address_policy.h"

#include <cassert>
#include <cstring>

namespace {

void TestHostApproachLifecycle()
{
    using namespace g3t::address_policy;

    // Active routine approach continues when NPC is alive, in routine (AIMode == 0), has navigation,
    // puppet is present, not timed out, and within range.
    HostNpcState active{};
    active.entityExists = true;
    active.hasNavigation = true;
    active.puppetExists = true;
    assert(DecideHostApproach(active, false) == HostApproachAction::Continue);

    // Remote client dialog takes precedence as StartedRemoteTalk without routine restoration.
    HostNpcState remoteTalk = active;
    remoteTalk.heldForRemoteTalk = true;
    assert(DecideHostApproach(remoteTalk, false) == HostApproachAction::StartedRemoteTalk);

    // Host PC_Hero dialog (e.g., guard theft confrontation) must cancel approach WITHOUT StartGoto or FullStop/ContinueRoutine.
    HostNpcState hostDialog = active;
    hostDialog.inHostDialog = true;
    hostDialog.aiMode = 6; // gEAIMode_Talk
    assert(DecideHostApproach(hostDialog, false) == HostApproachAction::CancelKeepState);

    // Even if InfoManager is not running yet/anymore, non-zero AIMode cancels without restoring routine.
    static constexpr int kBusyModes[] = {1, 3, 4, 5, 6, 8, 9, 10, 11, -1};
    for (int mode : kBusyModes) {
        HostNpcState busy = active;
        busy.aiMode = mode;
        assert(DecideHostApproach(busy, false) == HostApproachAction::CancelKeepState);
    }

    // Host dialog alone (even before AIMode flips from 0) must still cancel safely.
    HostNpcState hostDialogEarly = active;
    hostDialogEarly.inHostDialog = true;
    assert(DecideHostApproach(hostDialogEarly, false) == HostApproachAction::CancelKeepState);

    // Downed, mirrored, or missing navigation also cancel without forcing routine reset.
    HostNpcState downed = active;
    downed.isDown = true;
    assert(DecideHostApproach(downed, false) == HostApproachAction::CancelKeepState);

    HostNpcState mirrored = active;
    mirrored.isMirrored = true;
    assert(DecideHostApproach(mirrored, false) == HostApproachAction::CancelKeepState);

    HostNpcState noNav = active;
    noNav.hasNavigation = false;
    assert(DecideHostApproach(noNav, false) == HostApproachAction::CancelKeepState);

    // Missing/dead entity erases without calling routine restore.
    HostNpcState missing = active;
    missing.entityExists = false;
    assert(DecideHostApproach(missing, false) == HostApproachAction::EraseOnly);

    HostNpcState dead = active;
    dead.isDead = true;
    dead.inHostDialog = true;
    assert(DecideHostApproach(dead, false) == HostApproachAction::EraseOnly);

    // Puppet gone, expired, or too far while NPC is still in routine restores routine.
    HostNpcState noPuppet = active;
    noPuppet.puppetExists = false;
    assert(DecideHostApproach(noPuppet, false) == HostApproachAction::GiveUpAndRestore);

    assert(DecideHostApproach(active, true) == HostApproachAction::GiveUpAndRestore);

    HostNpcState tooFar = active;
    tooFar.tooFar = true;
    assert(DecideHostApproach(tooFar, false) == HostApproachAction::GiveUpAndRestore);
}

void TestHostAddressRequestValidation()
{
    using namespace g3t::address_policy;

    HostNpcState ok{};
    ok.entityExists = true;
    ok.puppetExists = true;
    ok.hasNavigation = true;
    assert(RejectHostAddressRequest(ok, false) == nullptr);
    assert(std::strcmp(RejectHostAddressRequest(ok, true), "already on its way") == 0);

    HostNpcState s = ok;
    s.tooFar = true;
    assert(std::strcmp(RejectHostAddressRequest(s, true), "too far") == 0);

    s.hasNavigation = false;
    assert(std::strcmp(RejectHostAddressRequest(s, true), "no navigation") == 0);

    s.aiMode = 6;
    assert(std::strcmp(RejectHostAddressRequest(s, true), "not in its routine") == 0);

    s.inHostDialog = true;
    assert(std::strcmp(RejectHostAddressRequest(s, true), "in a dialog") == 0);

    s.inHostDialog = false;
    s.heldForRemoteTalk = true;
    assert(std::strcmp(RejectHostAddressRequest(s, true), "in a dialog") == 0);

    s.isMirrored = true;
    assert(std::strcmp(RejectHostAddressRequest(s, true), "not simulated here") == 0);

    s.puppetExists = false;
    assert(std::strcmp(RejectHostAddressRequest(s, true), "no puppet") == 0);

    s.isDown = true;
    assert(std::strcmp(RejectHostAddressRequest(s, true), "gone") == 0);
}

void TestClientWaitCancellation()
{
    using namespace g3t::address_policy;

    ClientWaitState ok{true, false, false, false};
    assert(!ShouldCancelClientWait(ok));
    assert(ShouldCancelClientWait(ClientWaitState{false, false, false, false}));
    assert(ShouldCancelClientWait(ClientWaitState{true, true, false, false}));
    assert(ShouldCancelClientWait(ClientWaitState{true, false, true, false}));
    assert(ShouldCancelClientWait(ClientWaitState{true, false, false, true}));
}

} // namespace

int main()
{
    TestHostApproachLifecycle();
    TestHostAddressRequestValidation();
    TestClientWaitCancellation();
    return 0;
}
