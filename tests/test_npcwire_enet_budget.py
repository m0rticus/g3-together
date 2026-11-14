# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

SECTION_FLAGS = ("-ffunction-sections", "-fdata-sections")
TARGET = _testbuild.register_target(
    "npcwire_enet_budget",
    [_testbuild.unit("tests/npcwire_enet_budget_test.c", warnings="none",
                     includes=(_testbuild.ENET_INCLUDE_DIR,), extra_gnu_flags=SECTION_FLAGS),
     *_testbuild.enet_units(exclude=("compress.c", "host.c", "protocol.c", "win32.c"),
                            opt=None, warnings="none", extra_gnu_flags=SECTION_FLAGS)],
    toolchain="c", allow_msvc=False, libs=("ws2_32",),
    extra_link_gnu=("-Wl,--gc-sections",),
)


class NpcWireEnetBudgetTests(unittest.TestCase):
    def test_actual_enet_fragment_boundaries(self):
        executed = TARGET.run(timeout=30)
        self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)


if __name__ == "__main__":
    unittest.main()
