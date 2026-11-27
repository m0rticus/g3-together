# SPDX-License-Identifier: GPL-3.0-or-later
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

HARNESS = r'''
#include "guards_policy.h"
#include <cassert>

using namespace g3t::guards_policy;

int main()
{
    // Stale CurrentAttacker == puppet must NEVER hijack a guard talking to the local host hero
    // (SayGuardNews / _AI_SayGuardNews / DIA_EXCUSE / DIA_PROVOKE), which would otherwise swap
    // the enclave to the remote player's theft counters and discard host SuspectComment[3] updates.
    assert(SelectGuardStateActor(true, true, false, "ZS_Talk", false, true, false, true, true) ==
           StateActorSource::None);
    assert(SelectGuardStateActor(true, true, false, "ZS_Talk_Loop", false, true, false, true, true) ==
           StateActorSource::None);
    assert(SelectGuardStateActor(true, true, false, "ZS_Talk_End", false, false, false, false, true) ==
           StateActorSource::None);

    // Even a remote guide/companion talking directly to the local host hero must stay on local host context.
    assert(SelectGuardStateActor(true, true, false, "ZS_Talk", true, true, false, true, true) ==
           StateActorSource::None);
    // Remote companion finishing talk with cleared target still preserves ContextLeader.
    assert(SelectGuardStateActor(true, true, false, "ZS_Talk_End", true, false, false, false, true) ==
           StateActorSource::ContextLeader);

    // Held dialog for a remote puppet (talk.cpp sets CurrentTarget = puppet) binds Target as before.
    assert(SelectGuardStateActor(true, true, false, "ZS_Talk", false, true, true, false, false) ==
           StateActorSource::Target);
    assert(SelectGuardStateActor(true, true, false, "ZS_Talk_Loop", true, true, true, false, true) ==
           StateActorSource::Target);

    // Suspect/zone/weapon/damage follow-up states targeting local host (or cleared ZS_ObserveSuspect)
    // must not fall back to stale CurrentAttacker or ContextLeader.
    assert(SelectGuardStateActor(true, true, false, "ZS_ObserveSuspect", false, true, false, true, true) ==
           StateActorSource::None);
    assert(SelectGuardStateActor(true, true, false, "ZS_ObserveSuspect", true, true, false, true, true) ==
           StateActorSource::None);
    assert(SelectGuardStateActor(true, true, false, "ZS_ObserveSuspect", false, false, false, false, true) ==
           StateActorSource::None);
    assert(SelectGuardStateActor(true, true, false, "ZS_ObserveSuspect", false, true, true, false, false) ==
           StateActorSource::Target);

    assert(SelectGuardStateActor(true, true, false, "ZS_ReactToDamage", false, true, false, true, true) ==
           StateActorSource::None);
    assert(SelectGuardStateActor(true, true, false, "ZS_ReactToDamage", false, true, false, false, true) ==
           StateActorSource::None);
    assert(SelectGuardStateActor(true, true, false, "ZS_ReactToDamage", false, false, false, false, true) ==
           StateActorSource::Attacker);

    // ContinueRoutine / ZS_GuidePlayer / ZS_WaitForFreeDestination_Loop preserve ContextLeader & WaitNearest.
    assert(SelectGuardStateActor(true, true, false, "ContinueRoutine", true, true, false, true, true) ==
           StateActorSource::ContextLeader);
    assert(SelectGuardStateActor(true, true, false, "ZS_WaitForFreeDestination_Loop", false, true, false, true, true) ==
           StateActorSource::WaitNearest);
    assert(SelectGuardStateActor(true, true, false, "ZS_WaitForFreeDestination_Loop", true, false, false, false, false) ==
           StateActorSource::ContextLeader);

    // Shared arena opponent bound to ContextLeader (Client 2) must use local hero context when targeting Host Hero.
    assert(SelectGuardStateActor(true, true, false, "ZS_Attack", true, true, false, true, false) ==
           StateActorSource::None);
    assert(SelectGuardStateActor(true, true, false, "ZS_Attack_Attack", true, true, false, true, false) ==
           StateActorSource::None);
    assert(SelectGuardStateActor(true, true, false, "ZS_Attack", true, true, true, false, false) ==
           StateActorSource::ContextLeader);

    // fightmem::StateActor must never let stale CurrentAttacker == puppet hijack stumbles/standups targeting host.
    assert(SelectFightStateActor(true, false, false, true, true, false, true, true) ==
           StateActorSource::None);
    assert(SelectFightStateActor(true, false, true, false, true, false, true, true) ==
           StateActorSource::None);
    assert(SelectFightStateActor(true, false, true, false, false, false, false, true) ==
           StateActorSource::None);
    assert(SelectFightStateActor(true, false, false, true, true, true, false, false) ==
           StateActorSource::Target);
    assert(SelectFightStateActor(true, false, false, true, false, false, false, true) ==
           StateActorSource::Attacker);
    return 0;
}
'''

TARGET = _testbuild.register_target(
    "guards_policy",
    [_testbuild.inline_unit("guards_policy.cpp", HARNESS, warnings="strict", includes=(ROOT / "src",))],
    msvc_vcvars=False,
)


class GuardsTests(unittest.TestCase):
    def test_guards_policy_header_is_engine_free(self):
        policy = _testbuild.src_text("guards_policy.h")
        includes = re.findall(r"^\s*#\s*include\s+([^\r\n]+)", policy, re.M)
        for inc in includes:
            self.assertTrue(inc.startswith("<"), f"unexpected non-standard include in guards_policy.h: {inc}")
        self.assertNotIn("Game.h", policy)
        self.assertNotIn("Script.h", policy)

    def test_state_actor_policy_executes_without_engine(self):
        result = TARGET.run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_guards_cpp_wires_state_actor_policy_and_stays_under_1000_lines(self):
        guards = _testbuild.src_text("guards.cpp")
        self.assertIn('#include "guards_policy.h"', guards)
        self.assertIn("guards_policy::SelectGuardStateActor(", guards)
        self.assertIn("IsLocalPlayer(target)", guards)
        for name in ("guards_policy.h", "guards.cpp"):
            self.assertLess(len(_testbuild.src_text(name).splitlines()), 1000)


if __name__ == "__main__":
    unittest.main()
