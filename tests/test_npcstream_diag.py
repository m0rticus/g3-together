import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild


NPCSTREAM_DIAG_TARGET = _testbuild.register_target(
    "npcstream-diag",
    [
        _testbuild.unit(
            "tests/npcstream_diag_test.cpp",
            std="c++17",
            warnings="strict",
            defines=("NDEBUG",),
            includes=(ROOT / "tests",),
        )
    ],
)


def function_body(source, declaration):
    start = source.index(declaration)
    opening = source.index("{", start)
    depth = 1
    for index in range(opening + 1, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:index]
    raise ValueError(f"Unclosed function: {declaration}")


class NpcStreamDiagnosticsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.world = _testbuild.src_text("world.cpp")
        cls.npcs = _testbuild.src_text("npcs.cpp")
        cls.npc_send = _testbuild.src_text("npc_send.cpp")

    def test_engine_free_diagnostics(self):
        executed = NPCSTREAM_DIAG_TARGET.run(timeout=60)
        self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

    def test_retirement_callers_distinguish_stop_and_callback(self):
        stop = function_body(self.world, "void __fastcall StopSession(")
        self.assertRegex(stop, r"\bRetireWorld\s*\(\s*true\s*\)\s*;")
        self.assertRegex(
            self.world,
            r"void\s+OnWorldDeactivate\s*\(\s*\)\s*\{\s*RetireWorld\s*\(\s*false\s*\)\s*;\s*\}",
        )

    def test_tick_attempts_resume_before_paused_return(self):
        tick = function_body(self.world, "void Tick()")
        self.assertRegex(
            tick,
            r"if\s*\(\s*retirementGate\.Paused\(\)\s*\)\s*ResumeAfterNativeStop\(\s*\)\s*;"
            r"\s*if\s*\(\s*retirementGate\.Paused\(\)\s*\)\s*\{",
        )
        second_pause = tick.index("if (retirementGate.Paused()) {")
        paused_body = function_body(tick[second_pause:], "if (retirementGate.Paused())")
        self.assertRegex(paused_body, r"\breturn\s*;")

    def test_native_resume_requires_running_session_and_hero(self):
        resume = function_body(self.world, "void ResumeAfterNativeStop()")
        self.assertRegex(
            resume,
            r"if\s*\(\s*!session\.IsGameRunning\(\)\s*\|\|\s*"
            r"!session\.GetHero\(\)\s*\|\|\s*"
            r"!retirementGate\.ResumeAfterStop\(\s*true\s*\)\s*\)\s*return\s*;",
        )

    def test_no_direct_forced_roi_updates(self):
        lookup = "FourceFullROIUpdate@eCEntityAdmin"
        self.assertEqual(self.world.count(lookup), 1)
        self.assertIn(lookup, function_body(self.world, "bool MarkProcessingDirty("))
        self.assertNotRegex(self.world, r"(?:->|\.)\s*FourceFullROIUpdate\s*\(")
        self.assertNotRegex(self.world, r"\bFourceFullROIUpdate\s*\(")

    def test_sendowned_stream_instrumentation_without_bandwidth_gate(self):
        start = self.npcs.index("void SendOwned(")
        end = self.npcs.index("void TryPlayerBlow(", start)
        sendowned = self.npcs[start:end] + "\n" + self.npc_send
        for call in (
            "streamDiag.BeginTick(", "streamDiag.Candidates(", "streamDiag.Reach(",
            "streamDiag.Sent(", "streamDiag.Released(", "streamDiag.Flush(",
            "world::HostSimulationStatus()", "npcsend_candidates::Union(candidates)",
        ):
            with self.subTest(call=call):
                self.assertIn(call, sendowned)
        for drop in ("Outside", "Unchanged", "Away", "Paced", "Failed"):
            with self.subTest(drop=drop):
                self.assertRegex(sendowned, rf"streamDiag\.Result\([^;]*Drop::{drop}\b")
        self.assertNotRegex(sendowned, re.compile(r"budget|credit", re.IGNORECASE))


if __name__ == "__main__":
    unittest.main()
