import os
import shutil
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class PlayerWireTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++") or shutil.which("cl")
        if not compiler:
            raise unittest.SkipTest("A C++17 compiler is required for the standalone player codec tests")
        prefix = ROOT / "tests" / f"playerwire_{os.getpid()}"
        cls.executable = prefix.with_suffix(".exe")
        cls.object = Path(f"{prefix}.obj")
        cls.addClassCleanup(lambda: [path.unlink(missing_ok=True)
                                     for path in [cls.executable, cls.object]])

        source = ROOT / "tests" / "playerwire_test.cpp"
        if Path(compiler).stem.lower() == "cl":
            subprocess.run([compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
                            "/c", str(source), f"/Fo{cls.object}"],
                           cwd=ROOT, check=True, capture_output=True, text=True)
            subprocess.run([compiler, "/nologo", str(cls.object), f"/Fe{cls.executable}"],
                           cwd=ROOT, check=True, capture_output=True, text=True)
        else:
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-pedantic", str(source), "-o", str(cls.executable)],
                           cwd=ROOT, check=True, capture_output=True, text=True)

    def run_case(self, case):
        subprocess.run([str(self.executable), case], cwd=ROOT, check=True,
                       capture_output=True, text=True)

    def test_roundtrip_masks_commit_preview_and_reliable_changes(self):
        self.run_case("roundtrip")

    def test_loss_recovery_stale_and_keyframe_reset(self):
        self.run_case("recovery")

    def test_full_keyframe_cadence_and_sequence_wrap(self):
        self.run_case("wrap")

    def test_malformed_trailing_bounds_and_authoritative_id(self):
        self.run_case("malformed")


if __name__ == "__main__":
    unittest.main()
