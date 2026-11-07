# SPDX-License-Identifier: GPL-3.0-or-later
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class StoryRestorationTests(unittest.TestCase):
    def test_production_snapshot_and_runtime_wire_policy(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++17 compiler required")
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / "story-policy.exe"
            result = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 str(ROOT / "tests" / "story_policy_test.cpp"), "-o", str(executable)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_atomic_reward_free_admission_wiring(self):
        story = (ROOT / "src" / "story.cpp").read_text(encoding="utf-8")
        persist = (ROOT / "src" / "persist.cpp").read_text(encoding="utf-8")
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
        story = (ROOT / "src" / "story.cpp").read_text(encoding="utf-8")
        doors = (ROOT / "src" / "doors.cpp").read_text(encoding="utf-8")
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
        items = (ROOT / "src" / "items.cpp").read_text(encoding="utf-8")
        ai = (ROOT / "src" / "ai.cpp").read_text(encoding="utf-8")
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
