import os
from pathlib import Path
import shutil
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

_compiler = shutil.which(os.environ.get("CXX", "clang++")) or shutil.which("g++") or shutil.which("cl")
_warnings = "none" if _compiler and Path(_compiler).stem.lower() == "cl" else "strict"
_includes = (ROOT / "src",) if _warnings == "none" else ()
TARGET = _testbuild.register_target(
    "combat_timing",
    [_testbuild.unit(path, warnings=_warnings, includes=_includes) for path in (
        ROOT / "tests" / "combat_timing_test.cpp", ROOT / "src" / "snapshot.cpp"
    )],
    msvc_vcvars=False,
)


class CombatTimingTests(unittest.TestCase):
    def test_engine_free_combat_timing(self):
        executed = TARGET.run(timeout=20)
        self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)
        if executed.stdout:
            print(executed.stdout, end="")

    def test_try_local_blow_phase_gate_wiring(self):
        npcs = _testbuild.src_text("npcs.cpp")
        snapshot = _testbuild.src_text("snapshot.cpp")

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
