import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild


NPC_SNAPSHOT_TIMING_TARGET = _testbuild.register_target(
    "npc-snapshot-timing",
    [
        _testbuild.unit(
            "tests/npc_snapshot_timing_test.cpp",
            std="c++17",
            warnings="strict",
            includes=(ROOT / "tests",),
        ),
        _testbuild.unit(
            "src/snapshot.cpp",
            std="c++17",
            warnings="strict",
            includes=(ROOT / "src",),
        ),
    ],
)


class NpcSnapshotTimingTests(unittest.TestCase):
    def test_sparse_npc_keyframes_do_not_inflate_playback_delay(self):
        executed = NPC_SNAPSHOT_TIMING_TARGET.run(timeout=10)
        self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)


if __name__ == "__main__":
    unittest.main()
