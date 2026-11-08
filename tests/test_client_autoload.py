import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def source(name):
    return (ROOT / name).read_text(encoding="utf-8")


def body(text, start, end):
    return text.split(start, 1)[1].split(end, 1)[0]


class ClientAutoloadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("C++17 compiler required for engine-free client autoload tests")
        cls.folder = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.folder.cleanup)
        cls.executable = Path(cls.folder.name) / "client_autoload.exe"
        subprocess.run(
            [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
             str(ROOT / "tests" / "client_autoload_test.cpp"), "-o", str(cls.executable)],
            check=True, capture_output=True, text=True,
        )

    def test_production_decision_table(self):
        subprocess.run([str(self.executable)], check=True, capture_output=True, text=True)

    def test_no_wire_change(self):
        self.assertIn("Protocol = 78", source("src/saving_policy.h"))
        policy = source("src/client_autoload.h")
        self.assertNotIn("net.h", policy)
        self.assertNotIn("SendWorld", policy)

    def test_entry_goes_through_the_template_entry_point_only(self):
        persist = source("src/persist.cpp")
        # The template load/teardown stays in EnterTemplateWorld; autoload only queues through the public entry point.
        self.assertEqual(len(re.findall(r'EnterHostWorld\("autoload"\)', persist)), 1)
        self.assertEqual(persist.count("confirmedStart();"), 1)  # the first New Game of a launch (no template yet)
        step = body(persist, "bool ProcessAutoload()", "wchar_t const* AutoloadMenuText")
        self.assertLess(step.index("menuCommandDepth) return false"), step.index("autoloadState.Step("))
        self.assertLess(step.index("autoloadState.Step("), step.index('EnterHostWorld("autoload")'))
        self.assertIn("GetAsyncKeyState(VK_ESCAPE)", step)
        self.assertIn("partner::Focused()", step)
        for forbidden in ("worldCleanup", "game.Stop()", "originalLoad", "TemplateValid", "SaveClientTemplate", "RemoveTemplate"):
            self.assertNotIn(forbidden, step)
        first = body(persist, "bool FirstEntryNow(uint32_t now)", "// Outer module boundary")
        self.assertIn("autoloadState.Entered(ok, GetTickCount())", first)  # timers start after the loading screen
        self.assertLess(first.index("clientRejoin.Left();"), first.index("confirmedStart();"))
        for forbidden in ("worldCleanup", "originalLoad", "SaveClientTemplate"):
            self.assertNotIn(forbidden, first)
        module = source("src/Script_G3Together.cpp")
        process = body(module, "void Process() override", "void PostProcess()")
        self.assertLess(process.index("ProcessClientTransition()"), process.index("FrameScope frame"))

    def test_gate_holds_automatic_entries_until_approved(self):
        persist = source("src/persist.cpp")
        transition = body(persist, "bool ProcessClientTransition()", "bool BeginFrame()")
        self.assertLess(transition.index("if (ProcessAutoload()) return true;"),
                        transition.index("const bool idleCandidate = !clientRejoin.pending && !templateCapture.wanted"))
        gate = transition.index("if (!entryManual && !entryApproved) return false;")
        self.assertLess(transition.index("clientRejoin.CanRetire(boundary)"), gate)
        self.assertLess(gate, transition.index("EnterTemplateWorld(startedAt);"))
        self.assertLess(gate, transition.index("EntryPlan(quiet)"))
        on_world = body(persist, "void OnWorld(uint8_t from", "void HostTick")
        self.assertIn("entryApproved = autoloadAwaiting = false;", on_world)  # a newer epoch restarts the countdown
        self.assertIn("AutoloadAvailable() ? i18n::ClientHostWorldChanged : i18n::ClientChooseNewGame", on_world)
        # Hero state never gates: the snapshot has no downed/dead input.
        snapshot = body(persist, "autoload::Snapshot AutoloadSnapshot", "void CountdownText")
        self.assertNotIn("downed::", snapshot)
        self.assertNotIn("Dead", snapshot)
        self.assertIn("s.queued = clientRejoin.pending && !entryManual && !entryApproved;", snapshot)

    def test_manual_new_game_takes_over(self):
        persist = source("src/persist.cpp")
        command = body(persist, "void __fastcall NewGameCommandDetour", "bool __stdcall NewGameDetour")
        self.assertLess(command.index('AutoloadCancel("manual New Game")'), command.index("GetPlayer()"))
        self.assertEqual(command.count("entryManual = true;"), 2)
        self.assertIn("clientWorldEntered && HostWorldEntryAvailable()", command)
        self.assertIn('EnterHostWorld("new-game-command")', command)

    def test_menu_text_precedes_legacy_hint_and_completion_is_polled(self):
        persist = source("src/persist.cpp")
        hints = body(persist, "void StatusHints()", "void SetWorldCleanup")
        self.assertLess(hints.index("AutoloadMenuText"), hints.index("!config.host && clientRejoin.pending) {"))
        self.assertIn("menuButtonSeen", hints)
        step = body(persist, "bool ProcessAutoload()", "wchar_t const* AutoloadMenuText")
        self.assertIn("autoloadAwaiting && !clientRejoin.pending", step)
        self.assertIn("autoloadState.Entered(ok, now)", step)
        self.assertIn("autoload::Status::NoRoute && clientRejoin.pending", step)  # never leave a stale world frozen forever

    def test_config_flag_documented_and_default_on(self):
        self.assertIn("bool clientAutoload = true;", source("src/config.h"))
        config_cpp = source("src/config.cpp")
        self.assertIn('"client_autoload"', config_cpp)
        self.assertIn('config.clientAutoload = _stricmp(value, "off") != 0;', config_cpp)
        package = source("tools/package.py")
        self.assertIn("client_autoload", package.split("CLIENT_INI", 1)[1])
        self.assertIn("client_autoload", package.split("HOST_INI", 1)[1].split("CLIENT_INI", 1)[0])
        self.assertIn("client_autoload", source("docs/PLAYING.md"))
        self.assertIn("client_autoload", source("docs/PLAYTEST.md"))
        self.assertIn("client_autoload", source("docs/JOINING.md"))

    def test_messages_exist_in_every_language_with_matching_placeholders(self):
        header = re.search(r"enum Message\s*\{(.*?)\};", source("src/i18n.h"), re.S).group(1)
        enum_names = re.findall(r"^\s+(\w+),$", header, re.M)
        enum_names.append("MessageCount")
        new = [n for n in enum_names if n.startswith("ClientAutoload")]
        self.assertEqual(len(new), 9)
        table = body(source("src/i18n.cpp"), "constexpr Translation Messages[] = {", "static_assert(sizeof(Messages)")
        entries = re.findall(r'\{L"((?:[^"\\]|\\.)*)",\s*L"((?:[^"\\]|\\.)*)",\s*L"((?:[^"\\]|\\.)*)"\}', table)
        self.assertEqual(len(entries), len(enum_names) - 1)  # MessageCount is not a message
        by_name = dict(zip(enum_names, entries))
        for name in new:
            english, german, polish = by_name[name]
            self.assertTrue(english and german and polish, name)
            placeholders = [re.findall(r"%[a-z]+", text) for text in (english, german, polish)]
            self.assertEqual(placeholders[0], placeholders[1], name)
            self.assertEqual(placeholders[0], placeholders[2], name)
        self.assertEqual(re.findall(r"%[a-z]+", by_name["ClientAutoloadCountdown"][0]), ["%u"])
        self.assertEqual(re.findall(r"%[a-z]+", by_name["ClientAutoloadCountdownFresh"][0]), ["%u"])
        self.assertEqual(re.findall(r"%[a-z]+", by_name["ClientAutoloadRetry"][0]), ["%u"])
        self.assertEqual(re.findall(r"%[a-z]+", by_name["ClientAutoloadCanceled"][0]), [])


if __name__ == "__main__":
    unittest.main()
