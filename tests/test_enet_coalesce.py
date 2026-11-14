# SPDX-License-Identifier: GPL-3.0-or-later
import os
import shutil
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

_MSVC = not (shutil.which("clang") or shutil.which("gcc"))
_OPTIONS = dict(opt=None, warnings="warn" if _MSVC else "none",
                defines=("_WINSOCK_DEPRECATED_NO_WARNINGS",) if _MSVC else ())
TARGET = _testbuild.register_target(
    "enet_coalesce",
    [*_testbuild.enet_units(**_OPTIONS, extra_gnu_flags=("-std=c99",)),
     _testbuild.unit(ROOT / "tests" / "enet_coalesce_test.c", lang="c", std="c99",
                     includes=(_testbuild.ENET_INCLUDE_DIR,), **_OPTIONS)],
    toolchain="c", respect_env=False,
    libs=("ws2_32", "winmm") if os.name == "nt" else (),
)


class EnetCoalesceTests(unittest.TestCase):
    def test_vendored_enet_packs_small_cross_channel_messages(self):
        result = TARGET.run(timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
