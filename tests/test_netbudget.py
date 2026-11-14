import os
from pathlib import Path
import shutil
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild


NETBUDGET_POLICY_TARGET = _testbuild.register_target(
    "netbudget-policy",
    [
        _testbuild.unit(
            "tests/netbudget_test.cpp",
            std="c++17",
            opt="-O2",
            warnings="strict",
        )
    ],
)
NETBUDGET_ENET_TARGET = _testbuild.register_target(
    "netbudget-enet",
    [
        _testbuild.unit(
            "tests/netbudget_enet_test.cpp",
            std="c++17",
            opt="-O2",
            warnings="strict",
            includes=(ROOT / "thirdparty" / "enet" / "include",),
        ),
        *_testbuild.enet_units(
            exclude=("compress.c", "host.c", "protocol.c", "win32.c"),
            opt="-O2",
            warnings="none",
        ),
    ],
    libs=("ws2_32", "winmm"),
)


class NetBudgetTests(unittest.TestCase):
    def test_pure_policy_and_actual_enet_fragment_boundaries(self):
        policy = NETBUDGET_POLICY_TARGET.run(timeout=60)
        self.assertEqual(policy.returncode, 0, policy.stdout + policy.stderr)
        enet = NETBUDGET_ENET_TARGET.run(timeout=60)
        self.assertEqual(enet.returncode, 0, enet.stdout + enet.stderr)


if __name__ == "__main__":
    unittest.main()
