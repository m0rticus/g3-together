import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def source(name):
    return (ROOT / name).read_text(encoding="utf-8")


class ClientAdmissionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("C++17 compiler required for engine-free client admission tests")
        cls.folder = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.folder.cleanup)
        cls.executable = Path(cls.folder.name) / "client_admission.exe"
        subprocess.run(
            [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
             str(ROOT / "tests" / "client_admission_test.cpp"), "-o", str(cls.executable)],
            check=True, capture_output=True, text=True,
        )

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], capture_output=True, text=True)
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
        self.assertIn("Protocol = 79", source("src/saving_policy.h"))

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
        body = persist.split("bool HeroPending()", 1)[1].split("void OnWorldActivate()", 1)[0]
        self.assertIn("(pairedEnabled || !net::IsHost())", body)
        self.assertNotRegex(body, r"^\s*\{\s*return\s+pairedEnabled\s*&&")

    def test_player_entries_are_adopted_and_bounded(self):
        persist = source("src/persist.cpp")
        new_game = persist.split("bool __stdcall NewGameDetour", 1)[1].split("bool WriteAtomic", 1)[0]
        self.assertIn("autoloadState.Adopted(GetTickCount())", new_game)
        transition = persist.split("bool ProcessClientTransition()", 1)[1].split("bool BeginFrame()", 1)[0]
        self.assertIn("manualEntry && clientRejoin.fresh", transition)
        self.assertEqual(len(re.findall(r'EnterHostWorld\("autoload"\)', persist)), 1)  # the correction reuses the one entry point

    def test_snapshot_marks_adoptable_worlds_and_keeps_admission_in_flight(self):
        persist = source("src/persist.cpp")
        snapshot = persist.split("autoload::Snapshot AutoloadSnapshot", 1)[1].split("void CountdownText", 1)[0]
        self.assertIn("s.adoptable = ", snapshot)
        self.assertIn("clientRejoin.fresh || (ownCharacterReady && synced == hostWorld.epoch)", snapshot)
        self.assertIn("s.hooks = rejoinHooks", snapshot)
        self.assertIn("s.client = !net::IsHost();", snapshot)

    def test_unsupported_install_is_loud_on_both_roles(self):
        persist = source("src/persist.cpp")
        hints = persist.split("void StatusHints()", 1)[1].split("void SetWorldCleanup", 1)[0]
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
