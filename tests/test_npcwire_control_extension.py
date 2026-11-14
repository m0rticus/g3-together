import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

TARGET = _testbuild.register_target(
    "npcwire_control_extension",
    ["src/npcwire_control.cpp", "tests/npcwire_control_extension_test.cpp"],
    pedantic=True, allow_msvc=False, respect_env=False,
)


class NpcWireControlExtensionTests(unittest.TestCase):
    def test_metadata_overrides(self):
        TARGET.run(check=True)


if __name__ == "__main__":
    unittest.main()
