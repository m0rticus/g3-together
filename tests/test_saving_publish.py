# SPDX-License-Identifier: GPL-3.0-or-later
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

SAVING_PUBLISH_TARGET = _testbuild.register_target(
    "saving-publish",
    [_testbuild.unit(
        "tests/saving_publish_test.cpp",
        std="c++17",
        warnings="strict",
    )],
)


class SavingPublishTests(unittest.TestCase):
    def test_production_publish_faults_cleanup_and_retry(self):
        with tempfile.TemporaryDirectory() as folder:
            slot = Path(folder) / "slot"
            result = SAVING_PUBLISH_TARGET.run(args=[str(slot)], cwd=folder)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
