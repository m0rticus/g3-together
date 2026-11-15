import os
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild


def source(name):
    return _testbuild.src_text(name)


def between(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


class CombatHostilityContractTests(unittest.TestCase):
    def test_focus_priority_native_prologue_matches_cp175(self):
        script_game = os.environ.get("G3T_SCRIPT_GAME_DLL")
        if not script_game:
            self.skipTest("Set G3T_SCRIPT_GAME_DLL for read-only native prologue checks")
        data = _testbuild.cached_bytes(script_game)
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        sections, = struct.unpack_from("<H", data, pe + 6)
        opt, = struct.unpack_from("<H", data, pe + 20)
        table = pe + 24 + opt
        rva = 0xAA4A0
        raw_bytes = None
        for i in range(sections):
            _, virt, srva, rsz, raw = struct.unpack_from("<8sIIII", data, table + i * 40)
            if srva <= rva < srva + max(virt, rsz):
                raw_bytes = data[raw + rva - srva:raw + rva - srva + 12]
                break
        self.assertEqual(raw_bytes, bytes.fromhex("55 8b ec 83 e4 f8 81 ec fc 03 00 00"))

    def test_focus_priority_hook_and_shared_hostility_wiring(self):
        ch = source("combat_hostility.cpp")
        self.assertIn("0xAA4A0", ch)
        self.assertIn("GetFocusEntityPriority", ch)
        self.assertIn("ai::JumpTo(", ch)
        self.assertIn("PS_Melee", ch)
        self.assertIn("PS_Ranged", ch)
        self.assertIn("PS_Magic", ch)
        self.assertIn("CombatFocusPriorityOverride(", ch)
        self.assertIn("ActiveSharedCombatHostility(", ch)
        self.assertIn("net::SendWorldState(s, 0)", ch)
        # Zero artificial rate-caps or throttles on hostility replication.
        self.assertNotIn("RateLimit", ch)
        self.assertNotIn("Throttle", ch)

        relation = source("relation.cpp")
        self.assertIn("combat_hostility::Tick()", relation)
        self.assertIn("combat_hostility::MirrorSharesCombatHostility(npc)", relation)
        self.assertIn("o.sharedHostile = combat_hostility::IsSharedCombatHostile(entity);", relation)

        ai = source("ai.cpp")
        self.assertIn("combat_hostility::MirrorSharesCombatHostility(judge)", ai)
        self.assertIn("combat_hostility::ShouldPromoteAttitude(judge)", ai)
        self.assertIn("combat_hostility::EffectiveAttitude(judge,", ai)

        net_cpp = source("net.cpp")
        self.assertIn("ClientWorldStateKind(state.kind)", net_cpp)
        self.assertIn("npclease::RemoteOwner(npc) != from", ch)

        npcs = source("npcs.cpp")
        self.assertIn("combat_hostility::SyncHostNpc(instance);", npcs)
        self.assertIn("combat_hostility::MirrorAttitude(instance, wanted)", npcs)

        infocmd = source("infocmd.cpp")
        self.assertIn("combat_hostility::OnState(from, s);", infocmd)

        main = source("Script_G3Together.cpp")
        self.assertIn("combat_hostility::OnWorldChange", main)

        cmake = _testbuild.cached_text(ROOT / "CMakeLists.txt")
        self.assertIn("src/combat_hostility.cpp", cmake)

    def test_bidirectional_client_lease_and_melee_integration(self):
        lease_rt = source("npclease_runtime.cpp")
        self.assertIn("bool LeasedCombatHostile(eCEntity const* entity)", lease_rt)
        activate = between(lease_rt, "bool Activate(", "Interest InterestOf(")
        self.assertIn("ResumeLeasedCombat(", activate)

        lease_client = source("npclease_client.cpp")
        self.assertIn("combat_hostility::CoopTargetPlayer(target)", lease_client)
        self.assertIn("combat_hostility::SyncClientLeasedNpc(entity);", lease_client)
        self.assertIn("combat_hostility::ClearClientLeasedNpc(key.data());", lease_client)
        self.assertIn("state.aiMode != entry.lease.state.aiMode", lease_client)
        self.assertIn("state.targetPlayer != entry.lease.state.targetPlayer", lease_client)

        melee = source("combat_melee.cpp")
        judge = between(melee, "MeleeVerdict JudgeMelee(", "void LogMelee(")
        self.assertIn("combat_hostility::EffectiveAttitude(victim, v.attitude)", judge)
        self.assertIn("combat_hostility::EffectiveAttitude(victim, result.relation)", judge)

        claims = source("combat_claims.cpp")
        self.assertIn("combat_hostility::SyncHostNpc(target.GetInstance());", claims)

    def test_touched_src_files_under_1000_lines(self):
        touched = (
            "relation_policy.h", "combat_hostility.h", "combat_hostility.cpp",
            "relation.cpp", "ai.cpp", "npcs.cpp", "npclease_runtime.h",
            "npclease_runtime.cpp", "npclease_client.cpp", "combat_melee.cpp",
            "combat_claims.cpp", "infocmd.cpp", "Script_G3Together.cpp",
        )
        for name in touched:
            lines = len(source(name).splitlines())
            self.assertLess(lines, 1000, f"{name} has {lines} lines (must be < 1000)")


if __name__ == "__main__":
    unittest.main()
