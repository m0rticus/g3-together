import os
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

POLICY_TARGET = _testbuild.register_target(
    "options_policy", ["tests/options_policy_test.cpp"],
    pedantic=True, allow_msvc=False, respect_env=False,
)
TRANSPORT_TARGET = _testbuild.register_target(
    "options_transport",
    [_testbuild.unit("tests/options_transport_test.cpp", pedantic=True,
                     includes=(_testbuild.ENET_INCLUDE_DIR,)),
     *_testbuild.NPCWIRE_UNITS],
    allow_msvc=False, respect_env=False,
)


class OptionsPolicyTests(unittest.TestCase):
    def test_production_classification_adoption_restore_matrix(self):
        POLICY_TARGET.run(check=True)

    def test_overlays_do_not_mutate_or_save_native_options(self):
        source = _testbuild.src_text("options.cpp")
        self.assertIn("effective = local;", source)
        self.assertIn("policy::ApplySetup(effective, session.host);", source)
        self.assertIn("policy::ApplyMod(config, session.host);", source)
        self.assertIn("policy::IsSetupSerializer(caller - gameBase)", source)
        for forbidden in ("const_cast", "SetValue(", "WriteFile(", "WritePrivateProfile", "SaveConfigFiles("):
            self.assertNotIn(forbidden, source)
        combat = _testbuild.src_text("combat.cpp")
        self.assertIn("options::NpcJudgement difficulty(!net::IsHost() && npcs::OwnerOf(victim.GetInstance()) == 1);", combat)
        scope = combat.index("options::NpcJudgement difficulty(")
        self.assertLess(combat.index("GELPVoid recipientArg = which == 0 ? other : self;"), scope)
        self.assertIn('persist::EnterHostWorld("host-balancing")', source)
        self.assertIn("session.epoch == epoch && newValues", source)
        self.assertIn("policy::NeedsEntry(newValues,", source)

    def test_production_network_delegation_and_identity_refusals(self):
        result = TRANSPORT_TARGET.run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_wire_adopts_before_admission_and_preserves_protocol(self):
        net = _testbuild.src_text("net.cpp")
        self.assertIn("Protocol = 80", _testbuild.src_text("saving_policy.h"))
        self.assertIn("struct Hello { uint8_t type; uint16_t version; char name[24]; uint64_t parity[parity::ItemCount]; };", net)
        welcome = net.split("} else if (type == MsgWelcome", 1)[1].split("} else if", 1)[0]
        self.assertLess(welcome.index("options::Adopt"), welcome.index("connected = true"))
        world = net.split("} else if (type == MsgWorld &&", 1)[1].split("\nuint32_t HashWorld", 1)[0]
        self.assertLess(world.index("options::Adopt"), world.index("worldHandler(from, message.note)"))
        self.assertIn("peer != server || !connected", world)
        self.assertIn("ENET_PACKET_FLAG_RELIABLE", world)
        self.assertGreaterEqual(net.count("options::Restore();"), 5)
        parity = _testbuild.src_text("parity.cpp")
        self.assertIn("options::policy::Capability", parity)
        self.assertNotIn("fnv.AddValue(setup.", parity)
        self.assertIn('AddFile(other, dir + "\\\\Data\\\\" + f.name, buffer)', parity)

    def test_localized_summary_and_package_import_remain(self):
        texts = _testbuild.src_text("i18n.cpp")
        self.assertIn("your difficulty (%s) is kept", texts)
        self.assertIn("dein Schwierigkeitsgrad (%s) bleibt", texts)
        self.assertIn("twój poziom trudności (%s) pozostaje", texts)
        package = _testbuild.cached_text(ROOT / "tools" / "package.py")
        self.assertIn('CLIENT_INI = """', package)
        self.assertNotIn("both players need the same values, the join compares them", package)

    def test_installed_game_serializer_and_player_start_reader_bytes(self):
        path = os.environ.get("G3T_GAME_DLL")
        if not path:
            self.skipTest("G3T_GAME_DLL required for read-only native evidence")
        pe = _testbuild.cached_pe(path)
        self.assertEqual(pe.get_data(0x90b0e, 8), bytes.fromhex("0fb680ea01000050"))
        self.assertEqual(pe.get_data(0x173890, 6), bytes.fromhex("ff1568d02410"))
        imports = pe.imports_by_va_bytes()
        self.assertEqual(imports[0x1024d068], b"?GetValue@eCConfigFile@@QBE_NABVbCString@@0AAV2@@Z")
        self.assertEqual(pe.get_data(0x15920c, 7), bytes.fromhex("80b8ea01000001"))

    def test_installed_script_setup_option_read_offsets(self):
        path = os.environ.get("G3T_SCRIPT_GAME_DLL")
        if not path:
            self.skipTest("G3T_SCRIPT_GAME_DLL required for read-only native evidence")
        pe = _testbuild.cached_pe(path)
        for rva, expected in ((0x4339, "80b8ea01000001"), (0x4848, "80b8eb01000000"),
                              (0x4f15a, "8b90ec010000"), (0x4e488, "db80f0010000"),
                              (0x637c4, "80b8f401000001")):
            data = bytes.fromhex(expected)
            self.assertEqual(pe.get_data(rva, len(data)), data)

    def test_installed_setup_getter_and_difficulty_are_live_reads(self):
        game = os.environ.get("G3T_GAME_DLL")
        script = os.environ.get("G3T_SCRIPT_DLL")
        if not game or not script:
            self.skipTest("G3T_GAME_DLL and G3T_SCRIPT_DLL required for native reader bytes")
        engine = _testbuild.cached_pe(Path(game).with_name("Engine.dll"))
        rva = engine.exports()["?GetEngineSetup@eCApplication@@QBEABUeSSetupEngine@@XZ"]
        self.assertEqual(engine.get_data(rva, 7), bytes.fromhex("8d8198000000c3"))
        native_script = _testbuild.cached_pe(script)
        rva = native_script.exports()["?GetCurrentDifficulty@Entity@@SG?AW4EDifficulty@@XZ"]
        self.assertEqual(native_script.get_data(rva + 0x22, 6), bytes.fromhex("81c1e0010000"))
