import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class RagdollForceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++") or shutil.which("cl")
        if not compiler:
            raise unittest.SkipTest("An existing C++ compiler is needed for the engine-free force tests")
        build = ROOT / "build"
        build.mkdir(exist_ok=True)
        cls.temporary = tempfile.TemporaryDirectory(prefix="ragdoll-tests-", dir=build)
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        cls.executable = directory / "ragdoll_force_test.exe"
        source = ROOT / "tests" / "ragdoll_force_test.cpp"
        if Path(compiler).stem.lower() == "cl":
            command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
                       str(source), f"/Fe{cls.executable}", f"/Fo{directory / 'test.obj'}"]
        else:
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f"Force test compilation failed:\n{result.stdout}{result.stderr}")

    def run_case(self, case):
        subprocess.run([str(self.executable), case], check=True, capture_output=True, text=True)

    def test_force_is_independent_of_world_coordinates_and_contact_tracks_the_corpse(self):
        self.run_case("points")

    def test_native_forces_survive_and_larger_forces_keep_direction_within_the_bound(self):
        self.run_case("limit")

    def test_nonfinite_data_is_rejected_without_a_success_shaped_force(self):
        self.run_case("invalid")

    def test_host_gravity_late_duplicate_reload_and_new_life_decisions(self):
        self.run_case("once")


if __name__ == "__main__":
    unittest.main()
