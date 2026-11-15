# SPDX-License-Identifier: GPL-3.0-or-later
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

REPUTATION_POLICY_TARGET = _testbuild.register_target(
    "reputation-policy",
    [_testbuild.unit(
        "tests/reputation_policy_test.cpp",
        std="c++17",
        warnings="strict",
    )],
)

TOUCHED_SRC_FILES = (
    "reputation_policy.h",
    "story.cpp",
    "story.h",
    "enclaves.cpp",
    "enclaves.h",
    "infocmd.cpp",
    "net.h",
    "net_messages.cpp",
    "net_session.cpp",
)


class ReputationSyncTests(unittest.TestCase):
    def test_engine_free_reputation_policy_and_scenarios(self):
        with tempfile.TemporaryDirectory() as folder:
            result = REPUTATION_POLICY_TARGET.run(cwd=folder)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_policy_header_is_engine_free(self):
        policy = _testbuild.src_text("reputation_policy.h")
        includes = re.findall(r"^\s*#\s*include\s+([^\r\n]+)", policy, re.M)
        for inc in includes:
            self.assertTrue(inc.startswith("<"), f"unexpected non-standard include in reputation_policy.h: {inc}")
        self.assertNotIn("Game.h", policy)
        self.assertNotIn("Script.h", policy)

    def test_authoritative_story_and_enclave_wiring(self):
        net_h = _testbuild.src_text("net.h")
        net_messages = _testbuild.src_text("net_messages.cpp")
        net_session = _testbuild.src_text("net_session.cpp")
        story = _testbuild.src_text("story.cpp")
        enclaves = _testbuild.src_text("enclaves.cpp")
        infocmd = _testbuild.src_text("infocmd.cpp")
        saving_policy = _testbuild.src_text("saving_policy.h")

        # Wire protocol constants & pre-admission StoryHandshake admission.
        self.assertIn("StateReputation = 6", net_h)
        self.assertIn("EnclaveRepDelta = 6", net_h)
        self.assertIn("domain > StateReputation", net_messages)
        self.assertIn("kind <= StateReputation", net_session)
        self.assertIn("Protocol = 79", saving_policy)

        # Story domain ordering, pre-admission cutover mask 0x3f, client QuestApply fame guard, and host quest flush.
        self.assertIn('{net::StateReputation, "reputation", &ReputationReady, &ReputationSnapshot, &ReputationApply}', story)
        self.assertLess(story.index('net::StateQuests, "quests"'), story.index('net::StateReputation, "reputation"'))
        self.assertIn("replay.sent != 0x3f", story)
        self.assertIn("if (net::IsHost() && d->kind == net::StateReputation)", story)
        self.assertIn("if (d.kind == net::StateReputation && !net::IsHost()) return;", story)
        self.assertIn("if (d->kind == net::StateQuests) FlushReputation();", story)
        quest_apply = story[story.index("bool QuestApply("):story.index("void QuestRuntimeSnapshot(")]
        self.assertIn("const bool preserveRep = !net::IsHost() && ReputationReady();", quest_apply)
        self.assertIn("ReputationApply(repKey.c_str(), repVal);", quest_apply)

        # Enclaves: InWorld-gated session request, Personal-mode fame sync, crime-only client delta filter, dialog & liberation handlers.
        self.assertIn("reputation::UpdateSessionRequest(", enclaves)
        self.assertIn("net::InWorld(net::LocalId())", enclaves)
        self.assertIn("if (note.op == net::EnclaveRepDelta)", enclaves)
        self.assertIn("if (mode == Personal) {\n        enclave->AccessPlayerFame() = note.fame;", enclaves)
        self.assertIn("SameCrimes(", enclaves)
        self.assertIn("void OnHostEnclaveLiberated(", enclaves)

        # Dialog ModReputPol / ModReputEnc & town-liberation fame forwarding in infocmd.cpp.
        self.assertIn("reputation::ParseDialogReputationCommand(", infocmd)
        self.assertIn("enclaves::SendDialogReputationDelta(", infocmd)
        self.assertIn("reputation::ShouldAwardLiberationFame(oldStatus, newStatus)", infocmd)
        self.assertIn("enclaves::OnHostEnclaveLiberated(", infocmd)

    def test_touched_source_files_stay_strictly_under_1000_lines(self):
        for name in TOUCHED_SRC_FILES:
            line_count = len(_testbuild.src_text(name).splitlines())
            self.assertLess(line_count, 1000, f"src/{name} has {line_count} lines (must be < 1000)")


if __name__ == "__main__":
    unittest.main()
