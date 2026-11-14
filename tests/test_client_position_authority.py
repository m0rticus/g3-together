import os
from pathlib import Path
import sys
import unittest

from test_downed_policy import function_body
from test_hud_notices import read_rva


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

_guards = _testbuild.src_text("guards.cpp")
identify = function_body(_guards, r"\bbool\s+IsLocalPlayer\s*\(")
reject = function_body(_guards, r"\bbool\s+RejectRemoteWrite\s*\(")
HARNESS = f"""
#include "position_policy.h"
#include <cassert>
class eCEntity {{}};
static eCEntity hero, creature, puppet, npc;
static eCEntity* controlled = &hero;
struct gCSession {{
    static gCSession& GetInstance() {{ static gCSession s; return s; }}
    eCEntity* GetHero() {{ return &hero; }}
}};
namespace g3t::remote {{ eCEntity* ControlledBody() {{ return controlled; }} }}
static uint32_t GetTickCount() {{ return 10; }}
static unsigned notices = 0;
static void Logf(char const*, ...) {{ ++notices; }}
namespace g3t::guards {{
bool IsLocalPlayer(eCEntity const* entity) {{{identify}}}
bool RejectRemoteWrite(eCEntity const* entity, char const* writer) {{{reject}}}
}}
int main() {{
    using namespace g3t;
    for (char const* writer : {{"body::Drive", "body::Pacify", "body::Release",
                               "remote::UpdatePuppet", "remote::Despawn"}}) {{
        assert(guards::RejectRemoteWrite(&hero, writer));
        controlled = &creature;
        assert(guards::RejectRemoteWrite(&hero, writer));
        assert(guards::RejectRemoteWrite(&creature, writer));
        assert(!guards::RejectRemoteWrite(&puppet, writer));
        assert(!guards::RejectRemoteWrite(&npc, writer));
        assert(!guards::RejectRemoteWrite(nullptr, writer));
    }}
    assert(notices == 1);
    const float origin[3] = {{}}, zero[3] = {{}}, run[3] = {{600,0,0}};
    const float normal[3] = {{6,0,0}}, jump[3] = {{6,5,0}}, snap[3] = {{0,-16,0}};
    const float backwards[3] = {{-80,0,0}}, exact[3] = {{30,0,0}};
    assert(!movement::PositionDiscontinuity(origin, normal, run, run, 10000, false));
    assert(!movement::PositionDiscontinuity(origin, jump, run, run, 10000, true));
    assert(!movement::PositionDiscontinuity(origin, exact, zero, zero, 0, false));
    assert(movement::PositionDiscontinuity(origin, snap, zero, zero, 0, true));
    assert(!movement::PositionDiscontinuity(origin, snap, zero, zero, 0, false));
    assert(movement::PositionDiscontinuity(origin, backwards, run, zero, 10000, true));
    assert(!movement::PositionDiscontinuity(origin, backwards, run, run, 250000, true));
    const float falling[3] = {{0,-600,0}}, fall[3] = {{0,-60,0}};
    assert(!movement::PositionDiscontinuity(origin, fall, falling, falling, 100000, true));
    assert(movement::PositionDiscontinuity(origin, fall, zero, zero, 10000, true));
}}
"""
TARGET = _testbuild.register_target(
    "client_position_authority",
    [_testbuild.inline_unit("authority.cpp", HARNESS, includes=(ROOT / "src",))],
    respect_env=False,
)


class ClientPositionAuthorityTests(unittest.TestCase):
    def source(self, name):
        return _testbuild.src_text(name)

    def test_production_local_body_guard_and_discontinuity_policy(self):
        result = TARGET.run(timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_playback_guards_precede_every_writer(self):
        body = self.source("body.cpp")
        for name, writer in (
            ("Drive", "entity.CharacterMovement.StopMovement"),
            ("Pacify", "entity.Routine.FullStop"),
            ("Release", "entity.CharacterMovement.StopMovement"),
            ("PlayAniInstruction", "PlayScriptAni"),
        ):
            function = function_body(body, rf"\b(?:void|bool)\s+{name}\s*\(")
            self.assertLess(function.index("guards::RejectRemoteWrite"), function.index(writer), name)
        passive = function_body(self.source("npcplayback.cpp"), r"\bbool\s+Passive\s*\(")
        self.assertLess(passive.index("guards::IsLocalPlayer"), passive.index("entity == probe.target"))
        remote = self.source("remote.cpp")
        for name, writer in (("Despawn", "puppet.entity.Party"),
                             ("UpdatePuppet", "puppet.entity = Entity::Spawn")):
            function = function_body(remote, rf"\bvoid\s+{name}\s*\(")
            self.assertLess(function.index("guards::RejectRemoteWrite"), function.index(writer))
        update = function_body(remote, r"\bvoid\s+UpdatePuppet\s*\(")
        self.assertLess(update.index("id == net::LocalId()"), update.index("Puppet&"))
        self.assertLess(update.index('"remote::Spawn"'), update.index("body::Pacify"))
        forget = function_body(remote, r"\bvoid\s+Forget\s*\(")
        self.assertLess(forget.index('"remote::Forget"'), forget.index("npc.NPC.SetCurrentTarget"))
        self.assertLess(forget.index('"remote::Forget"'), forget.index("npc.Routine.FullStop"))

    def test_self_echo_and_metadata_resync_cannot_move_local_hero(self):
        net = self.source("net.cpp")
        store = function_body(net, r"\bvoid\s+Store\s*\(")
        self.assertLess(store.index("state.id == localId"), store.index("playerStateHandler(state"))
        state = net[net.index("} else if (type == MsgState)"):net.index("} else if (type == MsgControlId)")]
        self.assertLess(state.index("player == localId"), state.index(".Decode("))
        self.assertIn("i != player", state)
        resync = state[state.index("} else if (type == MsgPlayerResync"):]
        self.assertIn("forcePlayerFull[to][request.player] = true", resync)
        for writer in ("MoveTo", "SetPose", "SetToWorldMatrix", "StopMovement", "playerStateHandler("):
            self.assertNotIn(writer, resync)
        feed = function_body(self.source("remote.cpp"), r"\bvoid\s+FeedPlayer\s*\(")
        self.assertLess(feed.index("state.id == net::LocalId()"), feed.index("Puppet&"))

    def test_explicit_restore_and_interaction_paths_do_not_use_remote_playback_guard(self):
        hero = function_body(self.source("hero.cpp"), r"\bbool\s+LoadRecordImpl\s*\(")
        self.assertIn("self.MoveTo(body::Pose(*hero, pose.position, pose.rotation))", hero)
        self.assertNotIn("RejectRemoteWrite", hero)
        persist = function_body(self.source("persist.cpp"), r"\bvoid\s+Tick\s*\(")
        self.assertIn("receivedNote.epoch == hostWorld.epoch", persist)
        self.assertIn("receivedWelcome == net::WelcomeSerial()", persist)
        self.assertIn("ApplyCharacter(receivedRecord", persist)
        self.assertIn("clientRejoin.CanUseFresh", persist)
        for name in ("downed.cpp", "talk.cpp", "trade.cpp", "hero.cpp", "persist.cpp"):
            self.assertNotIn("RejectRemoteWrite", self.source(name), name)
        commands = self.source("infocmd.cpp")
        self.assertIn("else if (s.moved && !mirrored", commands)
        self.assertIn("e.MoveTo(body::Pose(*instance, s.position, s.rotation))", commands)

    def test_native_transform_observers_are_read_only_bounded_and_attributed(self):
        movement = self.source("movement.cpp")
        for name, original in (("NativeMatrix", "originalMatrix(self, edx, matrix)"),
                               ("NativePose", "originalPose(self, edx, matrix)")):
            function = function_body(movement, rf"\bvoid\s+__fastcall\s+{name}\s*\(")
            self.assertIn(original, function)
            self.assertLess(function.index(original), function.index("AfterTransform"))
            self.assertIn("_ReturnAddress()", function)
            self.assertNotIn("return", function)
        before = function_body(movement, r"\bSample\s+BeforeTransform\s*\(")
        self.assertIn("!guards::IsLocalPlayer(entity)", before)
        self.assertLess(before.index("persist::NativeLoading()"), before.index("guards::IsLocalPlayer"))
        after = function_body(movement, r"\bvoid\s+AfterTransform\s*\(")
        for mutation in ("Logf", "SetToWorldMatrix(", "SetPose(", "MoveTo(", "StopMovement(", "new "):
            self.assertNotIn(mutation, after)
        self.assertIn("positionWriteCount == positionWrites.size()", after)
        self.assertIn("std::array<PositionWrite, 8>", movement)
        drain = function_body(movement, r"\bvoid\s+DrainPositionWrites\s*\(")
        self.assertIn("Caller(write.caller", drain)
        self.assertIn("mode=%d->%d ani=%d->%d", drain)
        self.assertIn("transform_discontinuities=%u frame_discontinuities=%u dropped=%u", movement)

    def test_transform_observer_signatures_match_supplied_engine_bytes(self):
        game = os.environ.get("G3T_GAME_DLL")
        if not game:
            self.skipTest("Set G3T_GAME_DLL for read-only native transform signatures")
        data = _testbuild.cached_bytes(Path(game).with_name("Engine.dll"))
        self.assertEqual(read_rva(data, 0x227100, 6), bytes.fromhex("55 8b ec 83 e4 f8"))
        self.assertEqual(read_rva(data, 0x177E10, 6), bytes.fromhex("8b 44 24 04 8b 11"))
        self.assertIn("matrixEntry == engine + 0x227100", self.source("movement.cpp"))
        self.assertIn("poseEntry == engine + 0x177e10", self.source("movement.cpp"))


if __name__ == "__main__":
    unittest.main()
