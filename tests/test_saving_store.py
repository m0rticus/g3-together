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

SAVING_STORE_TARGET = _testbuild.register_target(
    "saving-store",
    [_testbuild.unit(
        "tests/saving_store_test.cpp",
        std="c++17",
        warnings="strict",
    )],
)


class SavingStoreTests(unittest.TestCase):
    def test_production_format_atomic_recovery_and_capture_flow(self):
        with tempfile.TemporaryDirectory() as folder:
            result = SAVING_STORE_TARGET.run(
                args=[str(Path(folder) / "test-save.g3t")],
                cwd=folder,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
