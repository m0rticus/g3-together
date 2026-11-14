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


NPCSEND_POLICY_TARGET = _testbuild.register_target(
    "npcsend-policy",
    [
        _testbuild.unit(
            "tests/npcsend_policy_test.cpp",
            std="c++17",
            opt="-O2",
            warnings="strict",
            includes=(ROOT / "tests",),
        )
    ],
)


class NpcSendPolicyTests(unittest.TestCase):
    def test_sendowned_candidate_inventory_contract(self):
        npcs = _testbuild.src_text("npcs.cpp")
        sendowned = npcs[npcs.index("void SendOwned("):npcs.index("void TryPlayerBlow(", npcs.index("void SendOwned("))]
        self.assertIn("Entity::GetNPCs()", sendowned)
        self.assertIn("world::ProcessedEntities()", sendowned)
        self.assertIn("eEPropertySetType_NPC", sendowned)
        self.assertIn("npcsend_candidates::Union(candidates)", sendowned)
        self.assertIn("for (GuidKey const& candidateKey : candidateKeys)", sendowned)

    def test_engine_free_policy_and_75_npc_simulation(self):
        executed = NPCSEND_POLICY_TARGET.run(timeout=60)
        self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)
        print(executed.stdout, end="")


if __name__ == "__main__":
    unittest.main()
