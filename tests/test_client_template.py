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

TARGETS = {
    name: _testbuild.register_target(
        Path(name).stem, [ROOT / "tests" / name], allow_msvc=False, respect_env=False
    )
    for name in ("client_template_test.cpp", "client_template_store_test.cpp")
}


class ClientTemplateTests(unittest.TestCase):
    def compile_and_run(self, source, needs_folder=False):
        if needs_folder:
            with tempfile.TemporaryDirectory() as folder:
                result = TARGETS[source].run([folder])
        else:
            result = TARGETS[source].run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_template_policy(self):
        self.compile_and_run("client_template_test.cpp")

    def test_template_store(self):
        self.compile_and_run("client_template_store_test.cpp", needs_folder=True)


if __name__ == "__main__":
    unittest.main()
