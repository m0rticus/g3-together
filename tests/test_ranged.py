import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild
from test_downed_policy import function_body


RANGED_CODE = r"""
#include "movement_policy.h"
#include "ranged_anim.h"

#include <cassert>
#include <cmath>
#include <cstring>

int main()
{
    using namespace g3t::ranged_anim;
    using g3t::net::PlayerFlags::InGame;
    using g3t::net::PlayerFlags::Robbable;

    // 1. Metadata round-trip & bit fields.
    OverlayMeta m{};
    m.ch0 = {2u, 3u, 1u, false, false, 3u};
    m.ch1 = {5u, 1u, 0u, true, true, 2u};
    m.weaponActive = true;
    m.weaponSeq = 3u;
    const std::uint32_t packedMeta = PackOverlayMeta(m);
    assert(packedMeta != 0u);
    const OverlayMeta um = UnpackOverlayMeta(packedMeta);
    assert(um.ch0.slot == 2u && um.ch0.blendMode == 3u && um.ch0.playMode == 1u &&
           !um.ch0.looping && !um.ch0.defaults && um.ch0.seq == 3u);
    assert(um.ch1.slot == 5u && um.ch1.blendMode == 1u && um.ch1.playMode == 0u &&
           um.ch1.looping && um.ch1.defaults && um.ch1.seq == 2u);
    assert(um.weaponActive && um.weaponSeq == 3u);

    // 2. Hot phase packing preserves non-negative int32_t (bit 31 == 0) across all byte values,
    //    including full weight1 bit 7 preserved in actionMs bit 8.
    for (int b : {0, 1, 64, 127, 128, 200, 254, 255}) {
        const auto qb = static_cast<std::uint8_t>(b);
        std::int32_t gold = 0;
        std::uint16_t actionMs = 0;
        const OverlayHot hotIn{qb, qb, qb, qb, qb};
        PackOverlayHot(hotIn, InGame, packedMeta, gold, actionMs);
        assert(gold >= 0 && (actionMs & HotActionMarker) != 0);
        const OverlayHot hotOut = UnpackOverlayHot(InGame, packedMeta, gold, actionMs);
        assert(hotOut.phase0 == qb && hotOut.weight0 == qb &&
               hotOut.phase1 == qb && hotOut.weight1 == qb &&
               hotOut.weaponPhase == qb);
    }

    // Robbable players preserve real gold instead of packing overlay hot phases.
    std::int32_t robbableGold = 777;
    std::uint16_t robbableAction = 12;
    PackOverlayHot({100u, 200u, 50u, 255u, 80u}, InGame | Robbable, packedMeta, robbableGold, robbableAction);
    assert(robbableGold == 777 && robbableAction == 12);

    // 3. One-shot draw progress advances monotonically; looping hold wraps cleanly across 1.0 -> 0.0.
    OverlayMeta aimMeta{};
    aimMeta.ch0 = {1u, 1u, 0u, false, false, 1u};
    aimMeta.ch1 = {2u, 1u, 0u, true, false, 1u};
    aimMeta.weaponActive = true;
    aimMeta.weaponSeq = 1u;
    const std::uint32_t aimBits = PackOverlayMeta(aimMeta);
    const OverlayHot h0{QuantizeUnit(0.20f), QuantizeUnit(0.20f), QuantizeUnit(0.90f), QuantizeUnit(0.80f), QuantizeUnit(0.10f)};
    const OverlayHot h1{QuantizeUnit(0.60f), QuantizeUnit(0.80f), QuantizeUnit(0.10f), QuantizeUnit(0.80f), QuantizeUnit(0.50f)};
    const OverlayHot mid = InterpolateOverlayHot(aimBits, 11u, 22u, 33u, h0, aimBits, 11u, 22u, 33u, h1, 0.5f);
    assert(std::fabs(DequantizeUnit(mid.phase0) - 0.40f) < 0.015f);
    assert(std::fabs(DequantizeUnit(mid.weight0) - 0.50f) < 0.015f);
    assert(DequantizeUnit(mid.phase1) < 0.02f || DequantizeUnit(mid.phase1) > 0.98f);
    assert(std::fabs(DequantizeUnit(mid.weaponPhase) - 0.30f) < 0.015f);

    // 4. Restart counter transition (aim -> shoot on same slot) does not blend incompatible clips.
    OverlayMeta shootMeta = aimMeta;
    shootMeta.ch0.seq = 2u;
    const std::uint32_t shootBits = PackOverlayMeta(shootMeta);
    const OverlayHot hShoot{QuantizeUnit(0.05f), QuantizeUnit(1.0f), 0u, 0u, QuantizeUnit(0.05f)};
    const OverlayHot switched = InterpolateOverlayHot(aimBits, 11u, 22u, 33u, h1, shootBits, 99u, 22u, 33u, hShoot, 0.5f);
    assert(switched.phase0 == h1.phase0);

    // 5. PuppetMotionHistory interpolates overlayHot alongside base locomotion.
    g3t::movement::PuppetMotionHistory history;
    g3t::movement::PuppetMotionSample s0{}, s1{}, out{};
    s0.sent = 2000;
    s0.overlayAnim0 = 11u;
    s0.overlayAnim1 = 22u;
    s0.weaponAnim = 33u;
    s0.overlayMeta = aimBits;
    s0.overlayHot = h0;
    history.Push(1, s0);

    s1 = s0;
    s1.sent = 2100;
    s1.overlayHot = h1;
    history.Push(1, s1);

    std::uint32_t ageMs = 0;
    assert(history.Sample(2050, out, ageMs));
    assert(out.overlayMeta == aimBits && out.overlayAnim0 == 11u);
    assert(std::fabs(DequantizeUnit(out.overlayHot.phase0) - 0.40f) < 0.02f);
    return 0;
}
"""

TARGET = _testbuild.register_target(
    "ranged_anim_unit",
    [_testbuild.inline_unit("ranged_anim_unit.cpp", RANGED_CODE, pedantic=True)],
    respect_env=False, msvc_vcvars=False,
)


class RangedAnimationSyncTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ranged_overlay = _testbuild.src_text("ranged_overlay.cpp")
        cls.body = _testbuild.src_text("body.cpp")
        cls.remote = _testbuild.src_text("remote.cpp")

    def test_overlay_packing_and_timeline_interpolation(self):
        executed = TARGET.run(timeout=30)
        self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

    def test_puppet_hands_link_arrow_or_bolt_when_bow_or_crossbow_held(self):
        sync_hands = function_body(self.ranged_overlay, r"\bvoid\s+SyncPuppetHands\s*\(")
        self.assertIsNotNone(sync_hands)
        self.assertIn("HoldStacks(leftStack, rightStack)", sync_hands)
        self.assertIn("HoldRightStack(idx)", sync_hands)
        self.assertIn("inv->LinkStackToSlot(idx, gESlot_RightHand, GETrue)", sync_hands)

        hold = function_body(self.remote, r"\bvoid\s+Hold\s*\(")
        self.assertIn("ranged::SyncPuppetHands", hold)

    def test_locomotion_timeline_preserves_active_ranged_overlays(self):
        drive_puppet = function_body(self.body, r"\bvoid\s+DrivePuppetTimeline\s*\(")
        self.assertIn("if (!target.keepOverlays) entity.StopPlayAni();", drive_puppet)
        self.assertIn("if (!target.keepOverlays) {", drive_puppet)

        update_puppet = function_body(self.remote, r"\bvoid\s+UpdatePuppet\s*\(")
        self.assertIn("target.keepOverlays = applyOverlays;", update_puppet)
        self.assertIn("ranged::PresentPuppetOverlay", update_puppet)
        self.assertIn("ranged::ClearPuppetOverlay", update_puppet)


if __name__ == "__main__":
    unittest.main()
