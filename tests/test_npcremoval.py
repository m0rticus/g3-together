import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class NpcRemovalTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("C++17 compiler required for engine-free NPC removal fixture")
        cls.folder = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.folder.cleanup)
        cls.executable = Path(cls.folder.name) / "npcremoval.exe"
        subprocess.run(
            [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
             str(ROOT / "tests" / "npcremoval_test.cpp"), "-o", str(cls.executable)],
            check=True, capture_output=True, text=True,
        )

    def run_fixture(self, section):
        subprocess.run([str(self.executable), section], check=True, capture_output=True, text=True)

    def test_atomic_name_configuration(self):
        self.run_fixture("names")

    def test_exact_world_instances_and_ardea_box(self):
        self.run_fixture("scope")

    def test_terminal_removal_and_rejoin_publication(self):
        self.run_fixture("lifecycle")


if __name__ == "__main__":
    unittest.main()
