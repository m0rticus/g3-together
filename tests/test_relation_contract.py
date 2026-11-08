import os
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"


def source(name):
    return (SRC / name).read_text(encoding="utf-8")


def between(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


class MinimalPe:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        sections, = struct.unpack_from("<H", self.data, pe + 6)
        optional_size, = struct.unpack_from("<H", self.data, pe + 20)
        self.base, = struct.unpack_from("<I", self.data, pe + 24 + 28)
        table = pe + 24 + optional_size
        self.sections = []
        for i in range(sections):
            _, virtual, rva, raw_size, raw = struct.unpack_from("<8sIIII", self.data, table + i * 40)
            self.sections.append((rva, max(virtual, raw_size), raw))

    def read(self, rva, size):
        for start, length, raw in self.sections:
            if start <= rva < start + length:
                return self.data[raw + rva - start:raw + rva - start + size]
        raise ValueError(hex(rva))

    def call_target(self, rva):
        instruction = self.read(rva, 5)
        if instruction[0] != 0xE8:
            return None
        return rva + 5 + struct.unpack("<i", instruction[1:])[0]


class RelationContractTests(unittest.TestCase):
    def test_native_name_colour_contract(self):
        script_game = os.environ.get("G3T_SCRIPT_GAME_DLL")
        game = os.environ.get("G3T_GAME_DLL")
        if not script_game or not game:
            self.skipTest("Set G3T_SCRIPT_GAME_DLL and G3T_GAME_DLL for read-only native checks")
        scripts = MinimalPe(script_game)
        # GetFocusNameAttitude (0xF340) is the name-colour value: it calls GetAttitude (0xE3B0) once, at 0xF3D7 (return 0xF3DC).
        self.assertEqual(scripts.read(0xF340, 12), bytes.fromhex("55 8b ec 83 e4 f8 81 ec fc 01 00 00"))
        self.assertEqual(scripts.call_target(0xF3D7), 0xE3B0)
        # The three writers of gCNPC_PS.AttitudeToPlayer2 and the focus priority reach it by direct calls.
        for call in (0x29713, 0x3154A, 0xA5F8E, 0xAA7A7, 0xAAE25):
            with self.subTest(call=hex(call)):
                self.assertEqual(scripts.call_target(call), 0xF340)
        # The renderer (Game.dll gCGUIEntityRenderer::GetAttitude 0x7A0C0) switches on the stored value: none, friendly, neutral,
        # angry, hostile, predator, panic.
        image = MinimalPe(game)
        table = struct.unpack("<7I", image.read(0x7A234, 28))
        self.assertEqual([target - image.base for target in table],
                         [0x7A19F, 0x7A1E8, 0x7A15D, 0x7A1A8, 0x7A1CC, 0x7A1B4, 0x7A1C0])

    def test_writer_query_is_exempt_from_the_downed_refusal_and_the_puppet_scope(self):
        ai = source("ai.cpp")
        filter_attitude = between(ai, "GEInt GE_STDCALL FilterAttitude(", "GEInt GE_STDCALL FilterContinue(")
        self.assertIn("_ReturnAddress()", filter_attitude)
        self.assertLess(filter_attitude.index("FocusNameAttitude(spu, self, other, args)"),
                        filter_attitude.index("RejectDownedTarget("))
        self.assertIn("relation::RefuseDownedAttitude(false, true) && RejectDownedTarget(", filter_attitude)
        writer = between(ai, "GEInt FocusNameAttitude(", "GEInt GE_STDCALL FilterAttitude(")
        for required in ("npcs::IsMirrored(judge)", "npcs::HostAttitude(judge, hostAttitude)",
                         "relation::FocusNameContext(", "guards::LocalPlayer local(true)", "remote::ControlledBody()",
                         "LedByRemotePlayer(judge)", "npcs::CompanionOwner(npc)"):
            self.assertIn(required, ai[ai.index("bool LedByRemotePlayer("):ai.index("GEInt GE_STDCALL FilterAttitude(")])
        self.assertNotIn("RejectDownedTarget", writer)
        self.assertNotIn("return gEAttitude_None", writer)
        install = between(ai, "if (!attitudeInstalled && gCSession", "if (gCSession::GetInstance().IsGameRunning()) downed::InstallTerminalGuards();")
        self.assertIn("if (attitudeNative) focusNameReturn = VerifiedFocusNameReturn();", install)
        verified = between(ai, "uintptr_t VerifiedFocusNameReturn()", "}\n\n}")
        for required in ("0xF3D7", "0xE3B0", "0xF340", "0xE8"):
            self.assertIn(required, verified)

    def test_local_attitude_is_unfiltered_and_local(self):
        ai = source("ai.cpp")
        local = between(ai, "bool LocalAttitude(", "void RelationStats(")
        self.assertIn("guards::LocalPlayer local(true)", local)
        self.assertIn("remote::ControlledBody()", local)
        self.assertNotIn("Entity::GetPlayer()", local)
        self.assertIn("++playerAttitudeQueries", local)
        self.assertIn("originalAttitude(nullptr, &npc, &player, 0)", local)
        self.assertNotIn("RejectDownedTarget", local)

    def test_downed_state_machine_never_writes_attitude(self):
        for name in ("downed.cpp", "downed.h", "downed_policy.h"):
            self.assertNotIn("AttitudeToPlayer2", source(name), name)

    def test_host_record_carries_the_healed_relation_and_never_overwrites_a_stored_one(self):
        npcs = source("npcs.cpp")
        self.assertIn("r.attitude = relation::Outgoing(instance, !self.IsDead());", npcs)
        relation = source("relation.cpp")
        outgoing = relation[relation.index("uint8_t Outgoing("):]
        self.assertLess(outgoing.index("if (stored != None || !alive) return"), outgoing.index("ai::LocalAttitude"))
        self.assertLess(outgoing.index("HealStored(stored, truth, alive)"), outgoing.index("AttitudeToPlayer2 ="))
        self.assertIn("HealIntervalMs", outgoing)

    def test_client_mirror_applies_the_host_relation_in_every_branch(self):
        npcs = source("npcs.cpp")
        tick = npcs[npcs.index("void PassiveMirrorTick()"):]
        unavailable = tick[tick.index("if (!HostPresentationAvailable(m.entity.GetInstance())) {"):]
        self.assertLess(unavailable.index('KeepHostAttitude(m, m.latest.attitude, "held")'),
                        unavailable.index("if (!m.staleHeld)"))
        unsampled = tick[tick.index("if (!body::Sample(m.body.track"):]
        self.assertLess(unsampled.index('KeepHostAttitude(m, m.latest.attitude, "unsampled")'), unsampled.index("++it;"))
        self.assertIn('KeepHostAttitude(m, shown.attitude, "shown")', tick)
        self.assertIn("m.attitudeWritten = relation::Unset;", tick)
        keep = between(npcs, "void KeepHostAttitude(", "void PassiveMirrorTick()")
        self.assertIn("relation::ApplyHost(stored, wanted, m.attitudeWritten)", keep)
        self.assertIn("relation: mismatch kind=native-drift", keep)
        # The attitude of a mirror is written nowhere else in the passive tick.
        self.assertEqual(tick.count("AccessAttitudeToPlayer2()"), 0)
        self.assertEqual(npcs.count("AccessAttitudeToPlayer2().AccessValue()"), 1)
        answer = between(npcs, "bool HostAttitude(", "bool RelationOf(")
        self.assertIn("relation::MirrorAnswersFromHost(", answer)
        self.assertIn("m.attitudeWritten != relation::Unset ? m.attitudeWritten : m.latest.attitude", answer)

    def test_diagnostics_are_wired_and_named(self):
        relation = source("relation.cpp")
        for line in ("relation: summary", "relation: transition", "relation: %s npc=", "relation: mismatch", "relation: change",
                     "relation: heal"):
            self.assertIn(line, relation)
        tick = relation[relation.index("void Tick()"):relation.index("uint8_t Outgoing(")]
        self.assertIn("downed::UnavailablePlayers()", tick)
        self.assertIn('"knocked-down"', tick)
        self.assertIn('"revive"', tick)
        self.assertIn("DiagnosticIntervalMs", tick)
        frame = source("Script_G3Together.cpp")
        self.assertLess(frame.index("g3t::fightmem::Tick()"), frame.index("g3t::relation::Tick()"))
        self.assertIn("src/relation.cpp", (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"))

    def test_wire_and_pose_codecs_are_untouched(self):
        for name in ("npcwire_pose.h", "npcwire_bonepack.h"):
            self.assertNotIn("relation", source(name))
        net = source("net.h")
        self.assertIn("uint8_t attitude = 0;    // protocol 71", net)
