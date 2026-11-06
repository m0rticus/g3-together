import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class NpcSnapshotTimingTests(unittest.TestCase):
    def test_sparse_npc_keyframes_do_not_inflate_playback_delay(self):
        compiler = shutil.which(os.environ.get("CXX", "clang++")) or shutil.which("g++") or shutil.which("cl")
        self.assertIsNotNone(compiler, "The engine-free C++ timing test needs a compiler")
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            executable = directory / ("timing.exe" if os.name == "nt" else "timing")
            sources = [str(ROOT / "tests" / "npc_snapshot_timing_test.cpp"), str(ROOT / "src" / "snapshot.cpp")]
            if Path(compiler).stem.lower() == "cl":
                command = [compiler, "/nologo", "/std:c++17", "/EHsc", *sources, f"/Fe:{executable}"]
            else:
                command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", *sources, "-o", str(executable)]
            compiled = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            executed = subprocess.run([str(executable)], cwd=directory, capture_output=True, text=True, timeout=10)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)


if __name__ == "__main__":
    unittest.main()
