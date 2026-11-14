# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

TARGETS = {
    "core_policies": _testbuild.register_target(
        "core_policies",
        [_testbuild.unit(path, includes=(ROOT / "src",)) for path in (
            ROOT / "tests" / "core_policies_test.cpp", ROOT / "src" / "guid_text.cpp"
        )],
        msvc_vcvars=False,
    ),
}


class CorePoliciesTests(unittest.TestCase):
    def test_damage_perception_processing_replication_and_guid_codec(self):
        executed = TARGETS["core_policies"].run(timeout=20)
        self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)


if __name__ == "__main__":
    unittest.main()
