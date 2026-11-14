import shutil
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

_WARNINGS = "warn" if shutil.which("clang++") or shutil.which("g++") else "none"
TARGET = _testbuild.register_target(
    "enet_range_coder",
    [_testbuild.unit(ROOT / "thirdparty" / "enet" / "compress.c", lang="c",
                     std="c11", opt="-O2", warnings=_WARNINGS,
                     includes=(_testbuild.ENET_INCLUDE_DIR,)),
     _testbuild.unit(ROOT / "tests" / "enet_range_coder_explore.cpp",
                     opt="-O2", warnings=_WARNINGS,
                     includes=(_testbuild.ENET_INCLUDE_DIR,))],
    respect_env=False,
)


class EnetRangeCoderExploreTests(unittest.TestCase):
    def test_range_coder_roundtrips_and_reports(self):
        result = TARGET.run(check=True)
        print(result.stdout.strip())


if __name__ == "__main__":
    unittest.main()
