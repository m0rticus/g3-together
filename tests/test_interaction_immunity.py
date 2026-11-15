import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_downed_policy import function_body


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

if str(ROOT / "tools") not in sys.path:
    sys.path.insert(0, str(ROOT / "tools"))
INTERACTION_TARGET = _testbuild.register_target(
    "interaction-immunity",
    [
        _testbuild.unit(
            "tests/interaction_immunity_test.cpp",
            std="c++17",
            warnings="strict",
            includes=(ROOT / "tests",),
        )
    ],
)


class InteractionImmunityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sources = {
            name: _testbuild.src_text(f"{name}.cpp")
            for name in ("downed", "remote", "net", "combat", "spells", "Script_G3Together")
        }

    def body(self, source, signature):
        result = function_body(self.sources[source], signature)
        self.assertIsNotNone(result, signature)
        return result

    def test_production_window_grace_symmetry_release_and_wire_policy(self):
        executed = INTERACTION_TARGET.run(timeout=20)
        self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

    def test_native_observation_is_live_bilateral_and_not_a_cached_task(self):
        state = self.body("downed", r"\buint8_t\s+LocalInteractionFlags\s*\(")
        for evidence in ("session.GetHero()", "NativeInteractionPage(session.GetGUIManager())",
                         "infos->AccessIsRunning()", "infos->AccessEndRequested()",
                         "infos->GetDialogPartner1() == hero", "infos->GetDialogPartnerNPC()",
                         "!Entity(partner).IsDead()", "DialogueObserved", "page == gEGamePage_Trade"):
            self.assertIn(evidence, state)
        for forbidden in ("IsHost()", "GetCurrentTask", "talk::", "trade::"):
            self.assertNotIn(forbidden, state[:state.index("Logf(")])
        harm = self.body("downed", r"\bbool\s+HarmProtected\s*\(")
        self.assertIn("LocalInteractionFlags() != 0", harm)
        self.assertIn("InteractionOf(entity)", harm)
        self.assertIn("entity == hero || entity == remote::ControlledBody()", harm)

    def test_native_main_page_layout_is_verified_before_private_reads(self):
        native = self.body("downed", r"\bint\s+NativeInteractionPage\s*\(")
        self.assertIn("GetProcAddress", native)
        self.assertIn("ANOMALY interaction page layout mismatch", native)
        self.assertLess(native.index("if (!supported)"), native.index("std::memcpy(&root"))
        for value in ("+ 4", "+ 0x84", "+ 0x88", "InteractionPage(page, beforeTutorial)"):
            self.assertIn(value, native)
        # Secondary overlays are not used as the main-page authority.
        self.assertEqual(native.count("manager->GetActivePage()"), 1)  # explicit unsupported-layout fallback only

    def test_shipped_native_getter_tutorial_and_open_close_evidence(self):
        path = os.environ.get("G3T_GAME_DLL")
        if not path:
            self.skipTest("Set G3T_GAME_DLL for read-only native byte verification")
        image = _testbuild.cached_pe(path)
        native = self.body("downed", r"\bint\s+NativeInteractionPage\s*\(")
        for name, address in (("pageBytes", 0x7B490), ("tutorialSelect", 0x8B359), ("tutorialSave", 0x8B384)):
            array = re.search(rf"\b{name}\[\]\s*=\s*\{{([^}}]+)\}}", native)
            self.assertIsNotNone(array)
            expected = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]+)", array[1]))
            self.assertEqual(image.get_data(address, len(expected)), expected)
        self.assertEqual(image.get_data(0x7B563, 2), b"\x6a\x06")  # OpenDialog selects 6
        self.assertEqual(image.get_data(0x7C007, 2), b"\x6a\x05")  # BeginTrade selects 5
        self.assertEqual(image.get_data(0x7C1BD, 2), b"\x6a\xff")  # CloseDialog selects -1
        self.assertEqual(image.get_data(0x1CD89E, 4), b"\xc6\x46\x14\x01")  # running before first info
        self.assertEqual(image.get_data(0x1CD2D0, 5), b"\xc6\x41\x1e\x01\xc3")  # end requested
        self.assertEqual(image.get_data(0x1CDBF2, 9), b"\x89\x5e\x44\x89\x5e\x48\x88\x5e\x14")

    def test_hero_and_world_safety_release(self):
        state = self.body("downed", r"\buint8_t\s+LocalInteractionFlags\s*\(")
        for condition in ("hero != interactionHero", "interaction.Clear()", "interactionWorldActive",
                          "session.IsGameRunning()", "!persist::NativeLoading()", "!persist::HeroPending()",
                          "!life.held", "!life.terminal", "!player.IsDead()", "!player.IsDown()"):
            self.assertIn(condition, state)
        for signature in (r"\bvoid\s+OnWorldChange\s*\(", r"\bvoid\s+OnWorldDeactivate\s*\("):
            self.assertIn("interaction.Clear()", self.body("downed", signature))
        deactivate = self.body("downed", r"\bvoid\s+OnWorldDeactivate\s*\(")
        self.assertIn("interactionWorldActive = false", deactivate)
        world = self.body("Script_G3Together", r"\bbEResult\s+WorldDeactivate\s*\(")
        self.assertIn("g3t::downed::OnWorldDeactivate()", world)

    def test_remote_uses_newest_activity_without_loaded_pose_or_distance(self):
        peer = self.body("downed", r"\buint8_t\s+PlayerInteractionFlags\s*\(")
        for condition in ("player == net::LocalId()", "LocalInteractionFlags()", "r.connected",
                          "r.activity.known", "r.activity.ticks", "r.activity.world",
                          "r.activity.flags", "PeerInteraction", "remote::CurrentWorld()"):
            self.assertIn(condition, peer)
        for forbidden in ("latest.flags", "latestTicks", "PuppetOf", "Distance", "InWorld("):
            self.assertNotIn(forbidden, peer)
        store = self.body("net", r"\bvoid\s+Store\s*\(")
        self.assertIn("r.activity.Accept", store)
        drop = self.body("net", r"\bvoid\s+Drop\s*\(")
        self.assertIn("remotes[id] = Remote{}", drop)

    def test_flag_changes_use_existing_immediate_reliable_heartbeat_and_pose(self):
        local = self.body("remote", r"\bbool\s+LocalState\s*\(")
        self.assertIn("downed::LocalInteractionFlags()", local)
        net = self.sources["net"]
        heartbeat = net[net.index("if (!activitySent || local.flags"):net.index("if (!haveLocal) return")]
        self.assertIn("local.flags != lastActivityFlags", heartbeat)
        self.assertIn("ChannelState, true", heartbeat)
        self.assertIn("now - lastActivitySend >= 1000", heartbeat)
        self.assertIn("local.flags != currentLocal.flags", net)
        receive = net[net.index("} else if (type == MsgPlayerActivity"):net.index("} else if (type == MsgState)")]
        self.assertIn("!downed::ValidActivityFlags(message.flags)", receive)
        self.assertIn("r.activity.Accept", receive)
        self.assertIn("sizeof(ActivityMsg) == 19", net)
        self.assertNotIn("message.flags & 128", receive)
        self.assertIn("ProtocolVersion = saving::Protocol", net)

    def test_host_rejects_damage_before_relay_and_local_sender_before_send(self):
        receive = self.sources["net"]
        receive = receive[receive.index("} else if (type == MsgHitPlayer"):receive.index("} else if (type == MsgGoods")]
        self.assertLess(receive.index("downed::PlayerInteractionFlags(message.to)"),
                        receive.index("Send(peers[message.to]"))
        send = self.body("net", r"\bvoid\s+SendHitPlayer\s*\(")
        self.assertLess(send.index("downed::PlayerInteractionFlags(playerId)"), send.index("SendToPeer("))
        claim = self.body("combat", r"\bvoid\s+OnHitNpc\s*\(")
        self.assertLess(claim.index('RejectDownedHit(target.GetInstance(), PlayerActor(from), "received-NPC-hit")'),
                        claim.index("JudgeDamage("))

    def test_native_down_mode_death_and_lethal_bookkeeping_cannot_bypass_immunity(self):
        mode = self.body("downed", r"\bvoid\s+__fastcall\s+SetMode\s*\(")
        for condition in ("requested == gEAIMode_Down", "requested == gEAIMode_Dead", "InteractionOf(entity)"):
            self.assertIn(condition, mode)
        self.assertLess(mode.index('RejectHarm(entity, nullptr, "SetAIMode")'), mode.index("originalSetMode("))
        death = self.body("downed", r"\bbool\s+RedirectDeath\s*\(")
        self.assertLess(death.index('RejectHarm(self, nullptr, "redirect-death")'), death.index("hero.Routine.FullStop()"))
        blow = self.body("downed", r"\bvoid\s+NoteBlow\s*\(")
        self.assertTrue(blow.strip().startswith("if (LocalInteractionFlags()) return;"))
        tick = self.body("downed", r"\bvoid\s+Tick\s*\(")
        protected = function_body(tick, r"\bif\s*\(\s*LocalInteractionFlags\(\)\s*\)")
        self.assertIn("lethalBlowAt = 0", protected)
        self.assertIn("lastHp = hero.PlayerMemory.GetHitPoints()", protected)

    def test_targeting_and_native_talk_routines_are_not_refactored(self):
        targeting = self.body("downed", r"\bbool\s+CombatUnavailable\s*\(\s*eCEntity")
        self.assertNotIn("Interaction", targeting)  # avoid FullStop of a native dialog partner
        self.assertIn("life.held", targeting)
        trade = _testbuild.src_text("trade.cpp")
        self.assertIn("manager->GetActivePage() == gEGamePage_Trade", trade)


if __name__ == "__main__":
    unittest.main()
