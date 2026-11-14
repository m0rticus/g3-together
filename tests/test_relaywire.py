import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

TARGET = _testbuild.register_target(
    "relaywire", ["tests/relaywire_test.cpp"],
    pedantic=True, respect_env=False, msvc_vcvars=False,
)


class RelayWireTests(unittest.TestCase):
    def test_exact_relay_envelope(self):
        result = TARGET.run()
        if result.returncode:
            raise RuntimeError(f"Relay wire test failed:\n{result.stdout}{result.stderr}")


if __name__ == "__main__":
    unittest.main()
