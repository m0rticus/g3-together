import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

WORLD_RETIREMENT_TARGET = _testbuild.register_target(
    "world-retirement",
    [_testbuild.unit(
        "tests/world_retirement_test.cpp",
        std="c++17",
        warnings="strict",
    )],
)


class WorldRetirementTests(unittest.TestCase):
    def test_native_owner_retirement_policy_without_engine(self):
        with tempfile.TemporaryDirectory() as folder:
            result = WORLD_RETIREMENT_TARGET.run(cwd=folder)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
