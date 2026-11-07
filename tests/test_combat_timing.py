import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class CombatTimingTests(unittest.TestCase):
    def test_engine_free_combat_timing(self):
        compiler = shutil.which(os.environ.get("CXX", "clang++")) or shutil.which("g++") or shutil.which("cl")
        self.assertIsNotNone(compiler, "The engine-free C++ combat timing test needs a compiler")
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            executable = directory / ("combat_timing.exe" if os.name == "nt" else "combat_timing")
            sources = [
                str(ROOT / "tests" / "combat_timing_test.cpp"),
                str(ROOT / "src" / "snapshot.cpp"),
            ]
            if Path(compiler).stem.lower() == "cl":
                command = [compiler, "/nologo", "/std:c++17", "/EHsc", *sources,
                           f"/Fe:{executable}", f"/I{ROOT / 'src'}"]
            else:
                command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", *sources,
                           "-o", str(executable)]
            compiled = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            executed = subprocess.run([str(executable)], cwd=directory, capture_output=True, text=True, timeout=20)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)
            if executed.stdout:
                print(executed.stdout, end="")

    def test_try_local_blow_phase_gate_wiring(self):
        npcs = (ROOT / "src" / "npcs.cpp").read_text(encoding="utf-8")
        snapshot = (ROOT / "src" / "snapshot.cpp").read_text(encoding="utf-8")

        start = npcs.index("void TryLocalBlow(")
        end = npcs.index("\nvoid MirrorTick()", start)
        function = npcs[start:end]
        instance_reset = function.index("if (m.contactInstance != instance)")
        hit_gate = function.index("combat_timing::HitPhaseReached")
        previous_sweep = function.index("const bool previous")
        self.assertLess(instance_reset, hit_gate)
        self.assertLess(hit_gate, previous_sweep)
        self.assertIn("m.contactInstance = instance", function)
        self.assertIn("m.contactConsumed = false", function)
        self.assertIn("return", function[hit_gate:previous_sweep])

        npcs_client_assignment = (
            "m.body.track.predictive = false;\n"
            "        m.body.track.combatPlayer = net::IsHost() ? 0 : net::LocalId();"
        )
        self.assertIn(npcs_client_assignment, npcs)

        snapshot_reset = (
            "const bool predictive = track.predictive;\n"
            "            const uint8_t combatPlayer = track.combatPlayer;"
        )
        self.assertIn(snapshot_reset, snapshot)
        self.assertIn("track.predictive = predictive;", snapshot)
        self.assertIn("track.combatPlayer = combatPlayer;", snapshot)
        self.assertIn("mirrors.clear();", npcs)
        self.assertIn("m.body.track.predictive = false;", npcs)


if __name__ == "__main__":
    unittest.main()
