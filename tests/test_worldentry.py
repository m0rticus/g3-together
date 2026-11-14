import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

WORLDENTRY_TARGET = _testbuild.register_target(
    "worldentry",
    [_testbuild.unit(
        "tests/worldentry_test.cpp",
        std="c++17",
        warnings="strict",
    )],
    pthread="always",
)


class WorldEntryTests(unittest.TestCase):
    def test_production_progress_and_report_cadence_without_engine(self):
        with tempfile.TemporaryDirectory() as folder:
            result = WORLDENTRY_TARGET.run(cwd=folder)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
