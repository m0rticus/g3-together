import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

TARGET = _testbuild.register_target(
    "client_autoload", [ROOT / "tests" / "client_autoload_test.cpp"],
    allow_msvc=False, respect_env=False,
    skip_message="C++17 compiler required for engine-free client autoload tests",
)


def source(name):
    if name == "src/persist.cpp":
        return "\n".join(
            _testbuild.src_text(part)
            for part in ("persist.cpp", "persist_hooks.cpp", "persist_storage.cpp", "persist_client.cpp")
        )
    return _testbuild.src_text(name[4:]) if name.startswith("src/") else _testbuild.cached_text(ROOT / name)


def body(text, start, end):
    return text.split(start, 1)[1].split(end, 1)[0]


def function_body(text, signature):
    for match in re.finditer(signature, text):
        opening = text.find("{", match.end())
        if opening < 0 or ";" in text[match.end():opening]:
            continue
        depth = 0
        for index in range(opening, len(text)):
            if text[index] == "{":
                depth += 1
            elif text[index] == "}":
                depth -= 1
                if not depth:
                    return text[opening + 1:index]
    raise AssertionError(f"missing function {signature}")


class ClientAutoloadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.executable = TARGET.executable()

    def test_production_decision_table(self):
        TARGET.run(check=True)

    def test_no_wire_change(self):
        self.assertIn("Protocol = 79", source("src/saving_policy.h"))
        policy = source("src/client_autoload.h")
        self.assertNotIn("net.h", policy)
        self.assertNotIn("SendWorld", policy)

    def test_entry_goes_through_the_template_entry_point_only(self):
        persist = source("src/persist.cpp")
        # The template load/teardown stays in EnterTemplateWorld; autoload only queues through the public entry point.
        self.assertEqual(len(re.findall(r'EnterHostWorld\("autoload"\)', persist)), 1)
        self.assertEqual(persist.count("confirmedStart();"), 1)  # the first New Game of a launch (no template yet)
        step = function_body(persist, r"\bbool\s+ProcessAutoload\s*\(")
        self.assertLess(step.index("menuCommandDepth) return false"), step.index("autoloadState.Step("))
        self.assertLess(step.index("autoloadState.Step("), step.index('EnterHostWorld("autoload")'))
        self.assertIn("GetAsyncKeyState(VK_ESCAPE)", step)
        self.assertIn("partner::Focused()", step)
        for forbidden in ("worldCleanup", "game.Stop()", "originalLoad", "TemplateValid", "SaveClientTemplate", "RemoveTemplate"):
            self.assertNotIn(forbidden, step)
        first = function_body(persist, r"\bbool\s+FirstEntryNow\s*\(")
        self.assertIn("autoloadState.Entered(ok, GetTickCount())", first)  # timers start after the loading screen
        self.assertLess(first.index("clientRejoin.Left();"), first.index("confirmedStart();"))
        for forbidden in ("worldCleanup", "originalLoad", "SaveClientTemplate"):
            self.assertNotIn(forbidden, first)
        module = source("src/Script_G3Together.cpp")
        process = body(module, "void Process() override", "void PostProcess()")
        self.assertLess(process.index("ProcessClientTransition()"), process.index("FrameScope frame"))

    def test_gate_holds_automatic_entries_until_approved(self):
        persist = source("src/persist.cpp")
        transition = function_body(persist, r"\bbool\s+ProcessClientTransition\s*\(")
        self.assertLess(transition.index("if (ProcessAutoload()) return true;"),
                        transition.index("const bool idleCandidate = !clientRejoin.pending && !templateCapture.wanted"))
        gate = transition.index("if (!entryManual && !entryApproved) return false;")
        self.assertLess(transition.index("clientRejoin.CanRetire(boundary)"), gate)
        self.assertLess(gate, transition.index("EnterTemplateWorld(startedAt);"))
        self.assertLess(gate, transition.index("EntryPlan(quiet)"))
        on_world = function_body(persist, r"\bvoid\s+OnWorld\s*\(")
        self.assertIn("entryApproved = autoloadAwaiting = false;", on_world)  # a newer epoch restarts the countdown
        self.assertIn("AutoloadAvailable() ? i18n::ClientHostWorldChanged : i18n::ClientChooseNewGame", on_world)
        # Hero state never gates: the snapshot has no downed/dead input.
        snapshot = function_body(persist, r"\bautoload::Snapshot\s+AutoloadSnapshot\s*\(")
        self.assertNotIn("downed::", snapshot)
        self.assertNotIn("Dead", snapshot)
        self.assertIn("s.queued = clientRejoin.pending && !entryManual && !entryApproved;", snapshot)

    def test_manual_new_game_takes_over(self):
        persist = source("src/persist.cpp")
        command = function_body(persist, r"\bvoid\s+__fastcall\s+NewGameCommandDetour\s*\(")
        self.assertLess(command.index('AutoloadCancel("manual New Game")'), command.index("GetPlayer()"))
        self.assertEqual(command.count("entryManual = true;"), 2)
        self.assertIn("clientWorldEntered && HostWorldEntryAvailable()", command)
        self.assertIn('EnterHostWorld("new-game-command")', command)

    def test_menu_text_precedes_legacy_hint_and_completion_is_polled(self):
        persist = source("src/persist.cpp")
        hints = function_body(persist, r"\bvoid\s+StatusHints\s*\(")
        self.assertLess(hints.index("AutoloadMenuText"), hints.index("!config.host && clientRejoin.pending) {"))
        self.assertIn("menuButtonSeen", hints)
        step = function_body(persist, r"\bbool\s+ProcessAutoload\s*\(")
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
