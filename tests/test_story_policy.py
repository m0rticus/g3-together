# SPDX-License-Identifier: GPL-3.0-or-later
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

STORY_POLICY_TARGET = _testbuild.register_target(
    "story-policy",
    [_testbuild.unit(
        "tests/story_policy_test.cpp",
        std="c++17",
        warnings="strict",
    )],
)


class StoryRestorationTests(unittest.TestCase):
    def test_production_snapshot_and_runtime_wire_policy(self):
        with tempfile.TemporaryDirectory() as folder:
            result = STORY_POLICY_TARGET.run(cwd=folder)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_atomic_reward_free_admission_wiring(self):
        story = _testbuild.src_text("story.cpp")
        persist = _testbuild.src_text("persist.cpp")
        commit = story[story.index("bool CommitBaselines()"):story.index("void OnStates(")]
        self.assertLess(commit.index("!d.baseline.complete"), commit.index("Apply(runtime"))
        self.assertIn("quest->AccessStatus() = static_cast<gEQuestStatus>(value)", story)
        self.assertIn("story::AdmissionReady() && doors::AdmissionReady()", persist)
        self.assertIn("!doors::SettleForSave()", persist)
        self.assertIn("StateQuestRuntime", story)
        self.assertIn("Apply(*d, entry.first.c_str(), entry.second, catchup)", story)
        self.assertIn("domain.snapshot(replay.values[domain.kind - 1])", story)
        self.assertIn("States delta = policy::Changes(replay.latest[d.kind - 1], current)", story)
        self.assertIn("StateWorldClock", story)
        self.assertIn("clock::RestoreAdmission(state)", story)
        self.assertIn("ready && hostAdmissionAck", persist)
        self.assertIn("answer.flags = net::WorldEntryConfirmed", persist)
        self.assertIn("!story::FinishAdmission(from) || !doors::FinishAdmission(from)", persist)

    def test_bootstrap_does_not_require_its_own_admission_gate(self):
        story = _testbuild.src_text("story.cpp")
        doors = _testbuild.src_text("doors.cpp")
        poll = story[story.index("void Poll("):story.index("void Start()")]
        self.assertIn("!net::Joined()", poll)
        self.assertNotIn("net::Connected()", poll)
        self.assertIn("!net::InWorld(net::LocalId())", poll)
        tick = doors[doors.index("void Tick()"):doors.index("void OnState(")]
        self.assertIn("!net::Joined()", tick)
        self.assertNotIn("net::Connected()", tick)
        ready = doors[doors.index("bool AdmissionReady()"):doors.index("bool FinishAdmission(")]
        self.assertIn("!baselines.count(key) || wanted.count(key)", ready)

    def test_personal_pickup_tombstones_cannot_grant_again(self):
        items = _testbuild.src_text("items.cpp")
        ai = _testbuild.src_text("ai.cpp")
        self.assertIn("if (!items::BeforeTake(spu)) return GETrue;", ai)
        before = items[items.index("bool BeforeTake("):items.index("void AfterTake(")]
        self.assertIn("if (consumed.count(key))", before)
        self.assertIn("return false;", before)
        tick = items[items.index("void TickPersonal()"):items.index("void OnWorldChange()")]
        self.assertIn("world::ProcessedEntities()", tick)
        self.assertIn("item.Decay()", tick)
        self.assertNotIn("retired.insert", tick)
        self.assertNotIn("Inventory", tick)


if __name__ == "__main__":
    unittest.main()
