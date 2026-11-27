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
    "client_admission", [ROOT / "tests" / "client_admission_test.cpp"],
    allow_msvc=False, respect_env=False,
    skip_message="C++17 compiler required for engine-free client admission tests",
)


def source(name):
    if name == "src/persist.cpp":
        return "\n".join(
            _testbuild.src_text(part)
            for part in ("persist.cpp", "persist_hooks.cpp", "persist_storage.cpp", "persist_client.cpp")
        )
    return _testbuild.src_text(name[4:]) if name.startswith("src/") else _testbuild.cached_text(ROOT / name)


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


class ClientAdmissionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.executable = TARGET.executable()

    def run_case(self, name):
        result = TARGET.run([name])
        self.assertEqual(result.returncode, 0, f"{name}: {result.stderr}")

    def test_scenarios(self):
        for name in (
            "host-first", "client-first", "fresh-adoption", "admission-never-completes",
            "menu-to-world-mismatch", "cancel-per-epoch", "autoload-off", "missing-route",
            "refused", "hooks-off", "host-menu", "host-loads-save", "rapid-epochs",
            "client-restart", "busy-restarts-countdown", "manual-new-game-adopted", "solo-world-then-connect",
        ):
            with self.subTest(scenario=name):
                self.run_case(name)

    def test_exhaustive_sweep(self):
        self.run_case("exhaustive-sweep")

    def test_policy_is_engine_free_and_in_world_removed(self):
        policy = source("src/client_autoload.h")
        self.assertEqual(re.findall(r"^\s*#\s*include\s+([^\r\n]+)", policy, re.M), ["<cstdint>"])
        self.assertNotRegex(policy, r"\bInWorld\b")

    def test_protocol_unchanged(self):
        self.assertIn("Protocol = 82", source("src/saving_policy.h"))

    def test_admission_logging(self):
        persist = source("src/persist.cpp")
        messages = (
            "admission: phase=", "admission: mismatch", "admission: correction started",
            "admission: correction finished", "admission: admitted", "admission: host epoch",
            "admission: connected",
        )
        self.assertEqual([message for message in messages if message not in persist], [])

    def test_hero_pending_covers_clients_without_paired_saves(self):
        persist = source("src/persist.cpp")
        body = function_body(persist, r"\bbool\s+HeroPending\s*\(")
        self.assertIn("(pairedEnabled || !net::IsHost())", body)
        self.assertNotRegex(body, r"^\s*return\s+pairedEnabled\s*&&")

    def test_player_entries_are_adopted_and_bounded(self):
        persist = source("src/persist.cpp")
        new_game = function_body(persist, r"\bbool\s+__stdcall\s+NewGameDetour\s*\(")
        self.assertIn("autoloadState.Adopted(GetTickCount())", new_game)
        transition = function_body(persist, r"\bbool\s+ProcessClientTransition\s*\(")
        self.assertIn("manualEntry && clientRejoin.fresh", transition)
        self.assertEqual(len(re.findall(r'EnterHostWorld\("autoload"\)', persist)), 1)  # the correction reuses the one entry point

    def test_snapshot_marks_adoptable_worlds_and_keeps_admission_in_flight(self):
        persist = source("src/persist.cpp")
        snapshot = function_body(persist, r"\bautoload::Snapshot\s+AutoloadSnapshot\s*\(")
        self.assertIn("s.adoptable = ", snapshot)
        self.assertIn("clientRejoin.fresh || (ownCharacterReady && synced == hostWorld.epoch)", snapshot)
        self.assertIn("s.hooks = rejoinHooks", snapshot)
        self.assertIn("s.client = !net::IsHost();", snapshot)

    def test_unsupported_install_is_loud_on_both_roles(self):
        persist = source("src/persist.cpp")
        hints = function_body(persist, r"\bvoid\s+StatusHints\s*\(")
        self.assertIn("HooksUnsupported()", hints)
        self.assertIn("UnsupportedText(", hints)
        self.assertIn("admission: unsupported install", hints)
        menu = hints.split("if (!running || inMenu) {", 1)[1]
        self.assertLess(menu.index("if (unsupported) {\n            if (hud::MenuLine"), menu.index("!refusal.empty()"))
        self.assertIn("if (!*reason && !refusal.empty())", hints)  # a new invite clears an old refusal
        self.assertIn("hud::Bad", hints)
        # the status line never degrades to the lobby text for a client in an unadmitted state
        self.assertIn("net::Connected() ? i18n::AdmissionInWorld : i18n::AdmissionJoining", hints)

    def test_host_says_waiting_versus_in_world(self):
        persist = source("src/persist.cpp")
        self.assertIn("HostNotice(i18n::HostClientWaiting, i, hud::Info)", persist)
        self.assertIn("HostNotice(i18n::HostClientInWorld, from, hud::Good)", persist)
        self.assertIn("admission: client peer=%u waiting %u s, not in the host's world", persist)

    def test_new_messages_exist_in_every_language_with_matching_placeholders(self):
        header = re.search(r"enum Message\s*\{(.*?)\};", source("src/i18n.h"), re.S).group(1)
        names = re.findall(r"^\s+(\w+),$", header, re.M)
        names.append("MessageCount")
        table = source("src/i18n.cpp").split("constexpr Translation Messages[] = {", 1)[1].split("static_assert(sizeof(Messages)", 1)[0]
        entries = re.findall(r'\{L"((?:[^"\\]|\\.)*)",\s*L"((?:[^"\\]|\\.)*)",\s*L"((?:[^"\\]|\\.)*)"\}', table)
        self.assertEqual(len(entries), len(names) - 1)
        by_name = dict(zip(names, entries))
        fresh = [n for n in names if n.startswith(("Admission", "HostClient"))]
        self.assertEqual(len(fresh), 13)
        for name in fresh + ["LobbyHosting", "LobbyWaiting", "LobbyJoined", "HostReached", "Reconnected"]:
            english, german, polish = by_name[name]
            self.assertTrue(english and german and polish, name)
            found = [re.findall(r"%[a-z]+", text) for text in (english, german, polish)]
            self.assertEqual(found[0], found[1], name)
            self.assertEqual(found[0], found[2], name)
        self.assertIn("NOT in the host's world yet", by_name["LobbyWaiting"][0])
        self.assertNotIn("Campaign ready", by_name["LobbyJoined"][0])

    def test_docs_describe_the_invariant_and_the_log_lines(self):
        for name in ("docs/JOINING.md", "docs/PLAYING.md", "docs/PLAYTEST.md"):
            text = source(name)
            self.assertIn("admission:", text, name)
        self.assertIn("never left playing silently", source("docs/PLAYING.md"))
        self.assertNotIn("never leaves the world", source("docs/PLAYING.md"))


if __name__ == "__main__":
    unittest.main()
