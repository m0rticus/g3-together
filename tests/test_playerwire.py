import subprocess
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

TARGET = _testbuild.register_target(
    "playerwire", ["tests/playerwire_test.cpp"],
    pedantic=True, respect_env=False, msvc_vcvars=False,
)

OVERLAY_CODE = r"""
#include "playerwire.h"
#include "ranged_anim.h"

#include <cassert>
#include <cmath>
#include <cstring>

int main()
{
    using g3t::net::PlayerFlags::InGame;
    using g3t::net::PlayerState;
    namespace pw = g3t::playerwire;
    namespace ra = g3t::ranged_anim;

    static_assert(pw::FixedBytes == 65);
    static_assert(pw::MetadataMask == 0x3fffu);

    pw::Encoder sender;
    pw::Decoder receiver;
    PlayerState s{};
    s.id = 2;
    s.sequence = 1;
    s.sent = 1000;
    s.flags = InGame;
    s.rotation[3] = 1.0f;
    s.gold = 550;
    std::strcpy(s.animation, "Hero_Stand_Bow_Arrow_Relax_N_01");
    std::strcpy(s.heldLeft, "It_Bow_Short_01");
    std::strcpy(s.heldRight, "Arrow");

    pw::Packet pkt0;
    assert(sender.Plan(s, s.sent, false, pkt0) == pw::Status::Ok);
    assert(pkt0.full && pkt0.reliable && pkt0.bytes.size() > pw::FixedBytes);
    sender.Commit(s, s.sent, pkt0);

    PlayerState out{};
    assert(receiver.Decode(pkt0.bytes.data(), pkt0.bytes.size(), out, 2) == pw::Status::Ok);
    assert(out.gold == 550 && out.overlayMeta == 0);
    assert(std::strcmp(out.heldRight, "Arrow") == 0);

    // Start aiming a bow: upper-body overlay + hold + bow bend animation + hot phases in gold/actionMs.
    ra::OverlayMeta meta{};
    meta.ch0 = {1u, 1u, 0u, false, false, 3u};
    meta.ch1 = {2u, 1u, 0u, true, false, 1u};
    meta.weaponActive = true;
    meta.weaponSeq = 2u;
    s.overlayMeta = ra::PackOverlayMeta(meta);
    std::strcpy(s.overlayAnim0, "Hero_Stand_Bow_Arrow_Aim_UpperBody_0_M_01");
    std::strcpy(s.overlayAnim1, "Hero_Stand_Bow_Arrow_Hold_UpperBody_0_N_01");
    std::strcpy(s.weaponAnim, "G3_Objects_Weapons_Bows_Shortbow_01_PowerRaise_00");

    ra::OverlayHot hot{
        ra::QuantizeUnit(0.42f),
        ra::QuantizeUnit(0.85f),
        ra::QuantizeUnit(0.75f),
        ra::QuantizeUnit(0.60f),
        ra::QuantizeUnit(0.42f),
    };
    ra::PackOverlayHot(hot, s.flags, s.overlayMeta, s.gold, s.actionMs);
    assert(s.gold >= 0 && (s.actionMs & ra::HotActionMarker) != 0);
    s.sequence = 2;
    s.sent = 1050;

    pw::Packet pktAim;
    assert(sender.Plan(s, s.sent, false, pktAim) == pw::Status::Ok);
    assert(!pktAim.full && pktAim.reliable && pktAim.bytes.size() > pw::FixedBytes);
    // Simulate unacked/uncommitted Plan: re-planning with new hot phase still includes metadata delta!
    hot.phase0 = ra::QuantizeUnit(0.80f);
    ra::PackOverlayHot(hot, s.flags, s.overlayMeta, s.gold, s.actionMs);
    s.sequence = 3;
    s.sent = 1100;
    pw::Packet pktRetry;
    assert(sender.Plan(s, s.sent, false, pktRetry) == pw::Status::Ok);
    assert(pktRetry.reliable && pktRetry.bytes.size() > pw::FixedBytes);
    sender.Commit(s, s.sent, pktRetry);

    assert(receiver.Decode(pktRetry.bytes.data(), pktRetry.bytes.size(), out, 2) == pw::Status::Ok);
    assert(out.overlayMeta == s.overlayMeta);
    assert(std::strcmp(out.overlayAnim0, s.overlayAnim0) == 0);
    assert(std::strcmp(out.overlayAnim1, s.overlayAnim1) == 0);
    assert(std::strcmp(out.weaponAnim, s.weaponAnim) == 0);
    const ra::OverlayHot unpacked = ra::UnpackOverlayHot(out.flags, out.overlayMeta, out.gold, out.actionMs);
    assert(unpacked.phase0 == hot.phase0 && unpacked.weight1 == hot.weight1 && unpacked.weaponPhase == hot.weaponPhase);

    // Subsequent hot-only frame carries 0 metadata bytes (FixedBytes == 65) and is unreliable!
    hot.phase0 = ra::QuantizeUnit(0.95f);
    ra::PackOverlayHot(hot, s.flags, s.overlayMeta, s.gold, s.actionMs);
    s.sequence = 4;
    s.sent = 1150;
    pw::Packet pktHotOnly;
    assert(sender.Plan(s, s.sent, false, pktHotOnly) == pw::Status::Ok);
    assert(!pktHotOnly.reliable && pktHotOnly.bytes.size() == pw::FixedBytes);
    sender.Commit(s, s.sent, pktHotOnly);
    assert(receiver.Decode(pktHotOnly.bytes.data(), pktHotOnly.bytes.size(), out, 2) == pw::Status::Ok);
    assert(out.overlayMeta == s.overlayMeta);

    // Clear overlay when lowering bow: metadata delta clears strings & overlayMeta, restoring gold.
    s.overlayMeta = 0;
    s.overlayAnim0[0] = '\0';
    s.overlayAnim1[0] = '\0';
    s.weaponAnim[0] = '\0';
    s.gold = 550;
    s.actionMs = 0;
    s.sequence = 5;
    s.sent = 1200;
    pw::Packet pktClear;
    assert(sender.Plan(s, s.sent, false, pktClear) == pw::Status::Ok);
    assert(pktClear.reliable);
    sender.Commit(s, s.sent, pktClear);
    assert(receiver.Decode(pktClear.bytes.data(), pktClear.bytes.size(), out, 2) == pw::Status::Ok);
    assert(out.overlayMeta == 0 && out.overlayAnim0[0] == '\0' && out.gold == 550);
    return 0;
}
"""

OVERLAY_TARGET = _testbuild.register_target(
    "playerwire_overlay",
    [_testbuild.inline_unit("playerwire_overlay.cpp", OVERLAY_CODE, pedantic=True)],
    respect_env=False, msvc_vcvars=False,
)


class PlayerWireTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.executable = TARGET.executable()

    def run_case(self, case):
        subprocess.run([str(self.executable), case], cwd=ROOT, check=True,
                       capture_output=True, text=True)

    def test_roundtrip_masks_commit_preview_and_reliable_changes(self):
        self.run_case("roundtrip")

    def test_loss_recovery_stale_and_keyframe_reset(self):
        self.run_case("recovery")

    def test_full_keyframe_cadence_and_sequence_wrap(self):
        self.run_case("wrap")

    def test_malformed_trailing_bounds_and_authoritative_id(self):
        self.run_case("malformed")

    def test_ranged_overlay_metadata_deltas_and_hot_phase_packing(self):
        executed = OVERLAY_TARGET.run(timeout=30)
        self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)


if __name__ == "__main__":
    unittest.main()
