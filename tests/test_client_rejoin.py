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
        self.assertLess(transition.index("EntryPlan(quiet)"), transition.index("EnterTemplateWorld(startedAt)"))
        refusal = transition.split("EnterTemplateWorld(startedAt);", 1)[1]
        self.assertIn("clientRejoin.failed = true", refusal)
        self.assertIn("TemplateImpossible(plan)", refusal)  # the exact reason, never a generic restart message
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
        self.assertIn('RemoveTemplate("native-load-failed")', enter.split("if (ok)", 1)[1])
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
        remove = source.split("bool RemoveTemplate(char const* reason)", 1)[1].split("// ---- one template save", 1)[0]
        self.assertIn("rejoin::tmpl::OwnSlot(rejoin::tmpl::Slot)", remove)
        self.assertIn("RemoveOwn(TemplatePaths()", remove)  # only the slot's own files and their .tmp staging files
        self.assertNotIn("lastSave", remove)
        self.assertNotIn("current", remove)
        start = source.split("bool TemplateStart(rejoin::tmpl::Origin origin, TemplateJob job)", 1)[1].split("// Each frame while a save settles", 1)[0]
        self.assertIn("tpl::OwnSlot(tpl::Slot)", start)
        self.assertLess(start.index("RemoveOwn(paths"), start.index("originalSave(manager, &name)"))
        self.assertEqual(start.count("originalSave(manager, &name)"), 1)  # exactly one native save per attempt
        self.assertIn("templatePublish.NativeReturned(nativeOk, lowerSaveCalls, lowerSaveGood", start)
        # The root cause: the files were judged right after the native call. The call only starts the settle phase now.
        for forbidden in ("SealFiles", "Observe(", "HasBytes", "Sleep(", "WriteAtomic("):
            self.assertNotIn(forbidden, start)
        poll = source.split("void TemplatePoll(uint32_t now)", 1)[1].split("// Silent regeneration", 1)[0]
        self.assertIn("templatePublish.Poll(facts, now)", poll)
        self.assertLess(poll.index("Observe(paths, true)"), poll.index("SealFiles("))
        self.assertNotIn("Sleep(", source.split("// ---- one template save", 1)[1].split("std::string CharacterPath()", 1)[0])
        ended = source.split("void TemplateEnded(uint32_t now)", 1)[1].split("// Native save of the running world", 1)[0]
        self.assertIn('RemoveTemplate("rollback")', ended)  # a failed save never leaves a half template
        self.assertNotIn("SaveDetour", start)
        loader = source.split("bool __stdcall NewGameDetour", 1)[1].split("bool WriteAtomic", 1)[0]
        native_call = loader.index("const bool ok = originalNewGame(self);")
        self.assertLess(loader.index("rejoin::NewGameAllowed(clientWorldEntered)"), native_call)
        self.assertIn("templateCapture.Want(GetTickCount())", loader[native_call:])
        self.assertIn("templateHeroFresh = true;", loader[native_call:])
        self.assertNotIn("SaveClientTemplate", source)
        transition = source.split("bool ProcessClientTransition()", 1)[1].split("bool BeginFrame()", 1)[0]
        self.assertEqual(transition.count("TemplateStart("), 3)  # background, first entry, host-load
        self.assertLess(transition.index("TemplatePoll(now)"), transition.index("if (!clientRejoin.pending) {"))
        tick = source.split("void Tick()\n{", 1)[1].split("namespace SaveExtensions", 1)[0]
        self.assertIn("const bool templateHold = templateCapture.Holding(now);", tick)
        self.assertLess(tick.index("const bool templateHold"), tick.index("if (newWorld && running && !templateHold) {"))
        self.assertEqual(tick.count("!templateCapture.Holding(now)"), 1)  # account-record restore waits for the save
        on_character = source.split("void OnCharacter(", 1)[1].split("void Evaluate()", 1)[0]
        self.assertIn("!templateCapture.Holding(GetTickCount())", on_character)
        self.assertIn("templateHeroFresh", on_character)  # a carried hero never starts a new account
        for guarded in ("needsNewHero && templateHeroFresh && clientRejoin.CanUseFresh", "!receivedRecord.empty() && !restoreFailed"):
            self.assertIn(guarded, tick)

    def test_restart_message_only_when_the_route_is_impossible(self):
        source = (ROOT / "src" / "persist.cpp").read_text()
        # The generic "restart Gothic 3" text survives only as the refusal of the hanging in-place New Game.
        self.assertEqual(source.count("i18n::ClientRestartToRejoin"), 1)
        new_game = source.split("bool __stdcall NewGameDetour", 1)[1].split("bool WriteAtomic", 1)[0]
        self.assertIn("i18n::ClientRestartToRejoin", new_game)
        # Every other refusal names its reason through the one notice function.
        self.assertEqual(source.count("TemplateImpossible(plan);"), 2)
        self.assertEqual(source.count("TemplateNotify("), 4)  # definition, failed save, impossible route, save-refused timeout
        text = source.split("wchar_t const* AdmissionText(", 1)[1].split("void AdmissionObserve(", 1)[0]
        self.assertIn("i18n::ClientTemplateUnavailable", text.split("case S::NoRoute", 1)[1])
        menu = source.split("wchar_t const* AutoloadMenuText(", 1)[1].split("void StatusHints()", 1)[0]
        self.assertIn("AdmissionText(autoloadStatus", menu)
        policy = (ROOT / "src" / "client_template.h").read_text()
        decide = policy.split("inline Plan Decide(Situation const& s)", 1)[1].split("// Silent regeneration", 1)[0]
        self.assertEqual(decide.count("Route::Impossible"), 5)

    def test_template_publish_waits_for_the_engines_world_write_in_the_log_contract(self):
        source = (ROOT / "src" / "persist.cpp").read_text()
        for line in ("template: state=", "template: save start", "template: native HUD.SaveGame", "template: settle +",
                     "template: ready slot=", "template: failed reason=", "template: file operation=", "template: remove reason=",
                     "template: refused reason=", "template: notice reason=", "template: startup slot="):
            self.assertIn(line, source)

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
