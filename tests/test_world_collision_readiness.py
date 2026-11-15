import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

WORLD_COLLISION_READINESS_TARGET = _testbuild.register_target(
    "world-collision-readiness",
    [_testbuild.unit(
        "tests/world_collision_readiness_test.cpp",
        std="c++17",
        warnings="strict",
    )],
)


class WorldCollisionReadinessTests(unittest.TestCase):
    def test_remote_sphere_static_collision_readiness_without_engine(self):
        with tempfile.TemporaryDirectory() as folder:
            result = WORLD_COLLISION_READINESS_TARGET.run(cwd=folder)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_world_hooks_gate_remote_npcs_on_static_collision_and_fix_database_slots(self):
        world_cpp = (ROOT / "src" / "world.cpp").read_text(encoding="utf-8")
        world_pvs_cpp = (ROOT / "src" / "world_pvs.cpp").read_text(encoding="utf-8")
        world_collision_cpp = (ROOT / "src" / "world_collision.cpp").read_text(encoding="utf-8")

        self.assertIn("pvs::ReadySimulationAnchors()", world_cpp)
        self.assertIn("collision::SyncSphereStaticCollision(admin, spheres.data(), sphereCount)", world_cpp)
        self.assertIn("collision::IsNpcCollisionReady(entity)", world_cpp)
        self.assertIn("collision::Retire()", world_cpp)

        self.assertIn("InstallDatabaseIndexFix()", world_pvs_cpp)
        self.assertIn("ShouldAttemptRegionRelease", world_pvs_cpp)
        self.assertIn("OnExitProcessingRange@eCEntity@@MAEXXZ", world_collision_cpp)
        self.assertIn("CacheOutInternal@eCEntity@@IAEX_N@Z", world_collision_cpp)
        self.assertIn("eETraceRayHints_TestStaticShapes | eETraceRayHints_IncludeWater", world_collision_cpp)


if __name__ == "__main__":
    unittest.main()
