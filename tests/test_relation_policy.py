from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

TARGET = _testbuild.register_target(
    "relation_policy", ["tests/relation_policy_test.cpp"], msvc_vcvars=False,
)


class RelationPolicyTests(unittest.TestCase):
    def test_production_relation_policy(self):
        result = TARGET.run(timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
