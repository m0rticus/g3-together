# SPDX-License-Identifier: GPL-3.0-or-later
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NpcWireEnetBudgetTests(unittest.TestCase):
    def test_actual_enet_fragment_boundaries(self):
        compiler = shutil.which(os.environ.get("CC", "clang")) or shutil.which("gcc")
        self.assertIsNotNone(compiler, "C compiler required for ENet fragmentation test")
        enet = ROOT / "thirdparty" / "enet"
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / ("enet_budget.exe" if os.name == "nt" else "enet_budget")
            sources = [ROOT / "tests" / "npcwire_enet_budget_test.c",
                       enet / "peer.c", enet / "list.c", enet / "packet.c", enet / "callbacks.c"]
            command = [compiler, "-std=c11", "-ffunction-sections", "-fdata-sections",
                       "-I", str(enet / "include"), *map(str, sources),
                       "-Wl,--gc-sections", "-o", str(executable)]
            if os.name == "nt":
                command.append("-lws2_32")
            compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            executed = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)


if __name__ == "__main__":
    unittest.main()
