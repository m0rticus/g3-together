import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
NET_CPP = ROOT / "src" / "net.cpp"
HERO_CPP = ROOT / "src" / "hero.cpp"
PERSIST_CPP = ROOT / "src" / "persist.cpp"


def function_body(source, signature):
    match = re.search(signature, source)
    if not match:
        return None
    opening = source.find("{", match.end())
    if opening < 0:
        return None
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:index]
    return None


class SavingPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.net = NET_CPP.read_text(encoding="utf-8")
        cls.hero = HERO_CPP.read_text(encoding="utf-8")
        cls.persist = PERSIST_CPP.read_text(encoding="utf-8")

    def test_campaign_bypasses_world_gate_only_as_save_notification(self):
        body = function_body(self.net, r"\bbool\s+Gated\s*\(\s*uint8_t\s+type\s*\)")
        self.assertIsNotNone(body)
        self.assertRegex(body, r"MsgCampaign\s*:")
        for exempt in ("MsgHello", "MsgWelcome", "MsgRefused", "MsgLeave", "MsgFileChunk", "MsgFileDone", "MsgWorld"):
            self.assertRegex(body, rf"\b{exempt}\b")

    def test_v3_embeds_spells_quick_and_validation_before_mutation(self):
        save_body = function_body(self.hero, r"\bbool\s+SaveRecord\s*\(\s*char\s+const\*\s+path\s*\)")
        load_body = function_body(self.hero, r"\bbool\s+LoadRecord\s*\(\s*char\s+const\*\s+path\s*\)")
        self.assertIsNotNone(save_body)
        self.assertIsNotNone(load_body)
        self.assertIn("G3THERO 3", save_body)
        self.assertIn("spell %s", save_body)
        self.assertIn("quick %d %s %u", save_body)
        self.assertIn("quick_end %zu %zu", save_body)
        self.assertIn("SaveQuick(self, path)", save_body)
        self.assertIn("quick_end", load_body)
        self.assertIn("declaredInlineSpells", load_body)
        self.assertIn("declaredInlineQuick", load_body)
        self.assertRegex(load_body, r"if\s*\(\s*!refused\s*\&\&\s*checking\s*\)")
        self.assertIn("ApplySpellAndQuick(self, path, inlineSpells, inlineQuick)", load_body)

    def test_snapshot_success_is_welcome_bound_and_file_done_refreshes_exact_record(self):
        on_file_done = function_body(
            self.persist,
            r"\bvoid\s+OnFileDone\s*\(\s*char\s+const\*\s+name\s*,",
        )
        on_campaign = function_body(
            self.persist,
            r"\bvoid\s+OnCampaign\s*\(\s*uint8_t\s+kind\s*,\s*char\s+const\*\s+name\s*\)",
        )
        self.assertIsNotNone(on_file_done)
        self.assertIsNotNone(on_campaign)
        self.assertIn("WelcomeSerial()", self.persist)
        self.assertIn("MarkSnapshotWritten", self.persist)
        self.assertIn("SnapshotWrittenInWelcome", on_file_done)
        self.assertIn("hero::SaveRecord(record.c_str())", on_file_done)
        self.assertIn("QueueSnapshot(record, net::WelcomeSerial(), false)", on_file_done)
        self.assertIn("QueueOrSaveSnapshot(record)", on_campaign)
        stable = function_body(
            self.persist,
            r"\bbool\s+StableRunningHero\s*\(\s*\)",
        )
        self.assertIsNotNone(stable)
        self.assertIn("pendingRecord.empty()", stable)
        self.assertIn("refusedRecord.empty()", stable)

    def test_host_save_and_load_validate_safe_names_before_native_and_path_use(self):
        save_body = function_body(
            self.persist,
            r"\bbool\s+__stdcall\s+SaveDetour\s*\(\s*void\*\s+self\s*,\s*bCString\s+const\*\s+name\s*\)",
        )
        load_body = function_body(
            self.persist,
            r"\bbool\s+__stdcall\s+LoadDetour\s*\(\s*void\*\s+self\s*,\s*bCString\s+const\*\s+name\s*\)",
        )
        safe_body = function_body(self.persist, r"\bbool\s+SafeName\s*\(\s*char\s+const\*\s+name\s*\)")
        self.assertIsNotNone(save_body)
        self.assertIsNotNone(load_body)
        self.assertIsNotNone(safe_body)
        self.assertLess(save_body.index("SafeName(name->GetText())"), save_body.index("originalSave"))
        self.assertLess(load_body.index("SafeName(name->GetText())"), load_body.index("originalLoad"))
        self.assertIn("n > 48", safe_body)
        self.assertIn("c == '-'", safe_body)
        self.assertIn("path + \".tmp\"", save_body)
        self.assertIn("MOVEFILE_REPLACE_EXISTING", save_body)

    def test_absent_received_sidecar_clears_the_local_old_file(self):
        on_file_done = function_body(
            self.persist,
            r"\bvoid\s+OnFileDone\s*\(\s*char\s+const\*\s+name\s*,",
        )
        self.assertIsNotNone(on_file_done)
        self.assertRegex(on_file_done, r"if\s*\(\s*!size\[2\]\s*\)")
        self.assertIn("DeleteFileA(optional.c_str())", on_file_done)


if __name__ == "__main__":
    unittest.main()
