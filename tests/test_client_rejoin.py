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
        self.assertNotIn("game.Stop()", transition)  # the player-less menu + native New Game froze the client
        self.assertNotIn("OpenMenu", transition)
        self.assertNotIn("CaptureCharacter", transition)
        self.assertNotIn("SaveRecord", transition)
        self.assertNotIn("worldCleanup();", transition)  # cleanup only inside the guarded template entry
        self.assertLess(transition.index("rejoin::ChooseRoute("), transition.index("EnterTemplateWorld(startedAt)"))
        refusal = transition.split("rejoin::ChooseRoute(", 1)[1].split("EnterTemplateWorld(startedAt)", 1)[0]
        self.assertIn("clientRejoin.failed = true", refusal)
        self.assertIn("ClientRestartToRejoin", refusal)
        self.assertIn("note.op == net::WorldOther", on_world)
        self.assertIn("clients[from].ready = clients[from].recordSent = clients[from].cutover = 0", on_world)

    def test_template_entry_uses_the_engine_load_path_in_order(self):
        source = (ROOT / "src" / "persist.cpp").read_text()
        enter = source.split("static void EnterTemplateWorld(", 1)[1].split("bool ProcessClientTransition()", 1)[0]
        self.assertLess(enter.index("++nativeLoadDepth"), enter.index("worldCleanup();"))
        self.assertLess(enter.index("worldCleanup();"), enter.index("originalLoad(files, &name)"))
        self.assertLess(enter.index("receivedRecord.clear()"), enter.index("originalLoad(files, &name)"))
        self.assertIn('crashlog::WorldEntryScope scope("native client template HUD.LoadGame")', enter)
        self.assertLess(enter.index("originalLoad(files, &name)"), enter.index("--nativeLoadDepth"))
        self.assertIn("lowerLoadCalls == 1 && lowerLoadGood", enter)
        self.assertIn("clientRejoin.Started(ok)", enter)
        self.assertIn("requestCharacterAgain = true", enter)
        self.assertIn("RemoveTemplate();", enter.split("if (ok)", 1)[1])
        self.assertNotIn("originalNewGame", enter)
        self.assertNotIn("Stop()", enter)
        self.assertNotIn("CapturePlayer", enter)
        entry = source.split("EnterResult EnterHostWorld(", 1)[1].split("// The engine's own LoadGame", 1)[0]
        self.assertIn("QueueClientReturn();", entry)  # queue only: no native call from ENet dispatch
        self.assertNotIn("originalLoad", entry)
        self.assertNotIn("worldCleanup", entry)
        header = (ROOT / "src" / "persist.h").read_text()
        self.assertIn("EnterResult EnterHostWorld(char const* reason);", header)
        self.assertIn("bool HostWorldEntryAvailable();", header)

    def test_template_slot_is_ours_and_saved_once_before_the_account_record(self):
        source = (ROOT / "src" / "persist.cpp").read_text()
        remove = source.split("void RemoveTemplate()", 1)[1].split("// One native save", 1)[0]
        self.assertIn("rejoin::tmpl::Slot", remove)
        self.assertNotIn("lastSave", remove)
        self.assertNotIn("current", remove)
        save = source.split("void SaveClientTemplate()", 1)[1].split("std::string CharacterPath()", 1)[0]
        self.assertIn("rejoin::tmpl::OwnSlot(rejoin::tmpl::Slot)", save)
        self.assertLess(save.index("RemoveTemplate();"), save.index("originalSave(manager, &name)"))
        self.assertIn("lowerSaveCalls == 1", save)
        self.assertLess(save.index("lowerSaveCalls == 1"), save.index("WriteAtomic("))
        self.assertIn("if (!ok) RemoveTemplate();", save)
        self.assertNotIn("SaveDetour", save)
        loader = source.split("bool __stdcall NewGameDetour", 1)[1].split("bool WriteAtomic", 1)[0]
        native_call = loader.index("const bool ok = originalNewGame(self);")
        self.assertLess(loader.index("rejoin::NewGameAllowed(clientWorldEntered)"), native_call)
        self.assertIn("templateCapture.Want(GetTickCount())", loader[native_call:])
        self.assertEqual(source.count("SaveClientTemplate();"), 1)
        tick = source.split("void Tick()\n{", 1)[1].split("namespace SaveExtensions", 1)[0]
        self.assertIn("const bool templateHold = templateCapture.Holding(now);", tick)
        self.assertLess(tick.index("const bool templateHold"), tick.index("if (newWorld && running && !templateHold) {"))
        self.assertEqual(tick.count("!templateCapture.Holding(now)"), 1)  # account-record restore waits for the save
        on_character = source.split("void OnCharacter(", 1)[1].split("void Evaluate()", 1)[0]
        self.assertIn("!templateCapture.Holding(GetTickCount())", on_character)
        for guarded in ("needsNewHero && clientRejoin.CanUseFresh", "!receivedRecord.empty() && !restoreFailed"):
            self.assertIn(guarded, tick)

    def test_native_new_game_is_not_the_world_replacement(self):
        # RestartWorld(false) on an active world is the in-place restart that never returned (build 42 freeze).
        source = (ROOT / "src" / "persist.cpp").read_text()
        self.assertNotIn("deferredNewGameMenu", source)
        command = source.split("void __fastcall NewGameCommandDetour", 1)[1].split("bool __stdcall NewGameDetour", 1)[0]
        self.assertIn("QueueClientReturn();", command)

    def test_world_entry_hang_is_located_without_a_dump(self):
        source = (ROOT / "src" / "crashlog.cpp").read_text()
        self.assertIn("world-entry: main thread stack operation=", source)
        poll = source.split("void PollWorldEntry()", 1)[1].split("void EndStep", 1)[0]
        self.assertLess(poll.index("LogMainThread(operation.name)"), poll.index("still waiting for"))
        sample = source.split("void SampleThread(", 1)[1].split("// A native call that never returns", 1)[0]
        self.assertLess(sample.index("SuspendThread"), sample.index("ResumeThread"))
        for forbidden in ("Logf", "Where(", "new ", "std::string", "malloc"):
            self.assertNotIn(forbidden, sample)

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
