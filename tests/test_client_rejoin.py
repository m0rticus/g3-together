import hashlib
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class ClientRejoinTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("C++17 compiler required for engine-free client rejoin tests")
        cls.folder = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.folder.cleanup)
        cls.executable = Path(cls.folder.name) / "client_rejoin.exe"
        subprocess.run(
            [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
             str(ROOT / "tests" / "client_rejoin_test.cpp"), "-o", str(cls.executable)],
            check=True, capture_output=True, text=True,
        )

    def test_production_state_policy_and_native_guards(self):
        subprocess.run([str(self.executable)], check=True, capture_output=True, text=True)

    def test_real_cp_menu_loader_and_aslr(self):
        path = os.environ.get("G3T_GAME_DLL")
        if not path:
            self.skipTest("Set G3T_GAME_DLL for read-only native rejoin checks")
        self.assertEqual(hashlib.sha256(Path(path).read_bytes()).hexdigest(),
                         "c7191a6ab0889fb2da38d56e683bf207d62c4bde6b6aa7ace136ee434f8b1d21")
        subprocess.run([str(self.executable), path], check=True, capture_output=True, text=True)

    def test_transition_runs_before_frame_and_outside_enet(self):
        module = (ROOT / "src" / "Script_G3Together.cpp").read_text()
        process = module.split("void Process() override", 1)[1].split("void PostProcess()", 1)[0]
        self.assertLess(process.index("ProcessClientTransition()"), process.index("FrameScope frame"))
        self.assertIn("ClientTransitionPending()", process)
        self.assertIn("g3t::offer::Tick();", process.split("ClientTransitionPending()", 1)[1])
        source = (ROOT / "src" / "persist.cpp").read_text()
        on_world = source.split("void OnWorld(uint8_t", 1)[1].split("void HostTick", 1)[0]
        self.assertNotIn("DespawnAll", on_world)
        self.assertNotIn("game.Stop()", on_world)
        transition = source.split("bool ProcessClientTransition()", 1)[1].split("bool BeginFrame()", 1)[0]
        self.assertLess(transition.index("worldCleanup();"), transition.index("game.Stop();"))
        self.assertNotIn("CaptureCharacter", transition)
        self.assertNotIn("SaveRecord", transition)
        self.assertIn("receivedRecord.clear()", transition)
        self.assertIn("requestCharacterAgain = true", transition)
        self.assertIn("note.op == net::WorldOther", on_world)
        self.assertIn("clients[from].ready = clients[from].recordSent = clients[from].cutover = 0", on_world)

    def test_only_successful_native_new_game_authorizes_fresh_hero(self):
        source = (ROOT / "src" / "persist.cpp").read_text()
        loader = source.split("bool __stdcall NewGameDetour", 1)[1].split("bool WriteAtomic", 1)[0]
        self.assertLess(loader.index("++nativeLoadDepth"), loader.index("originalNewGame(self);", loader.index("++nativeLoadDepth")))
        self.assertIn("clientRejoin.Started(ok)", loader)
        self.assertIn("worldCleanup();", loader)
        self.assertNotIn("CapturePlayer", loader)
        self.assertIn("clientRejoin.CanUseFresh", source)
        activate = source.split("void OnWorldActivate()", 1)[1].split("void OnWorldDeactivate()", 1)[0]
        self.assertIn("if (!net::IsHost()) return;", activate)


if __name__ == "__main__":
    unittest.main()
