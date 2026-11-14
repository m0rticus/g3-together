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

    def test_keepalive_focus_interval_and_capture_sanitization(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / "jitter_policy_check.cpp"
            cpp.write_text(r'''
#include "npcsend_policy.h"
#include <cassert>
#include <cmath>
int main() {
    using namespace g3t;
    static_assert(npcsend::KeepaliveMs == 180, "keepalive must stay under 200 ms mirror buffer ceiling");
    static_assert(npcsend::QuietCadenceMs == 250, "quiet cadence wire constant preserved");

    net::NpcRecord r{};
    r.incarnation = 1;
    r.rotation[3] = 1.0f;
    r.hitPoints = 100;
    r.hitPointsMax = 100;
    r.damageMultiplier = 1.0f;

    npcsend::State state{};
    npcsend::Input in{};
    in.now = 1000;
    in.record = &r;
    in.eligible = true;
    in.moving = true;
    in.nearby = true;
    in.distanceCm = 800.0f;
    in.focused = true;
    const auto first = npcsend::Evaluate(state, in);
    assert(first.decision == npcsend::Decision::Initial);
    assert(npcsend::Commit(state, in, first.decision));
    in.now = 1020;
    const auto second = npcsend::Evaluate(state, in);
    assert(second.tier == npcsend::Tier::Fast);
    assert(npcsend::PresentationInterval(state, in) == npcsend::OrdinaryIntervalMs);
    in.combat = true;
    assert(npcsend::PresentationInterval(state, in) == npcsend::FastIntervalMs);

    net::NpcRecord dirty{};
    dirty.incarnation = 0;
    dirty.attitude = 0xFF;
    dirty.aiMode = 0xFF;
    dirty.hitPointsMax = 0;
    dirty.damageMultiplier = NAN;
    dirty.motions[0].active = 1;
    dirty.motions[0].name = 0x12345678u;
    dirty.motions[0].instance = 7;
    dirty.motions[0].phase = 0.5f;
    dirty.motions[0].weight = 1.0f;
    dirty.motions[0].intro = -1.0f;
    dirty.motions[0].extro = -1.0f;
    npcsend::SanitizeCapturedRecord(dirty);
    npcsend::State cleanState{};
    in.record = &dirty;
    in.now = 2000;
    const auto sanitized = npcsend::Evaluate(cleanState, in);
    assert(sanitized.decision == npcsend::Decision::Initial);
    return 0;
}
''', encoding="utf-8")
            target = _testbuild.NativeTarget(
                "npcsend-jitter-policy-check",
                [_testbuild.unit(cpp, std="c++17", warnings="strict", includes=(ROOT / "src",))],
            )
            executed = target.run(timeout=30)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)


if __name__ == "__main__":
    unittest.main()
