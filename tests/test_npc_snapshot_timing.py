import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild


NPC_SNAPSHOT_TIMING_TARGET = _testbuild.register_target(
    "npc-snapshot-timing",
    [
        _testbuild.unit(
            "tests/npc_snapshot_timing_test.cpp",
            std="c++17",
            warnings="strict",
            includes=(ROOT / "tests",),
        ),
        _testbuild.unit(
            "src/snapshot.cpp",
            std="c++17",
            warnings="strict",
            includes=(ROOT / "src",),
        ),
    ],
)


class NpcSnapshotTimingTests(unittest.TestCase):
    def test_sparse_npc_keyframes_do_not_inflate_playback_delay(self):
        executed = NPC_SNAPSHOT_TIMING_TARGET.run(timeout=10)
        self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

    def test_pose_history_slerp_gap_reset_and_clamped_boundary(self):
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / "pose_jitter_check.cpp"
            cpp.write_text(r'''
#include "pose.h"
#include "npclease.h"
#include <cassert>
#include <cmath>
int main() {
    using namespace g3t;
    pose::History h;
    pose::Bone b0{}, b1{};
    b0.name = b1.name = 0xABCD01u;
    // Frame 0: identity rotation, translation (0, 0, 0)
    b0.values[0] = 4096; b0.values[4] = 4096; b0.values[8] = 4096;
    // Frame 1: +90 deg around Z (column-major: X=(0,1,0), Y=(-1,0,0), Z=(0,0,1)), translation (40, 0, 0) -> 10 cm
    b1.values[1] = 4096; b1.values[3] = -4096; b1.values[8] = 4096; b1.values[9] = 40;
    assert(h.Push(1, 1000, &b0, 1));
    assert(h.Push(1, 1100, &b1, 1));

    std::vector<pose::Transform> out;
    assert(h.Sample(1, 1050, out) && out.size() == 1);
    const float* v = out[0].values;
    const float xLen = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    const float yLen = std::sqrt(v[3] * v[3] + v[4] * v[4] + v[5] * v[5]);
    // Linear matrix LERP would shrink axes to ~0.7071; SLERP preserves unit length (+45 deg around Z).
    assert(std::fabs(xLen - 1.0f) < 0.002f && std::fabs(yLen - 1.0f) < 0.002f);
    assert(std::fabs(v[0] - 0.70710678f) < 0.003f && std::fabs(v[1] - 0.70710678f) < 0.003f);
    assert(std::fabs(v[3] + 0.70710678f) < 0.003f && std::fabs(v[4] - 0.70710678f) < 0.003f);
    assert(std::fabs(v[9] - 5.0f) < 0.01f);

    // Clamped boundary sampling holds 1-packet skew without failing, while rejecting far-outside timestamps.
    assert(h.SampleClamped(1, 1120, 300, 300, out));
    assert(std::fabs(out[0].values[9] - 10.0f) < 0.01f);
    assert(!h.SampleClamped(1, 1500, 300, 300, out));

    // Gap > GapResetMs clears pre-walk-away frames instead of blending across multi-second streaming gaps.
    assert(h.Push(1, 1100 + pose::GapResetMs + 50, &b0, 1));
    assert(h.Size() == 1);
    assert(!h.Sample(1, 1200, out));

    // DiscardBefore drops stale pre-handback frames while retaining same-tick or newer post-handback frames.
    assert(h.Push(1, 2800, &b1, 1));
    h.DiscardBefore(1, 2800);
    assert(h.Size() == 1 && h.LatestSent() == 2800);

    // Same-tick Commit echo == Initial sent must NOT be considered strictly older than presentationFloor.
    const uint32_t floor = 50000;
    assert(!npclease::Newer(floor, 50000));
    assert(!npclease::Newer(floor, 50016));
    assert(npclease::Newer(floor, 49984));
    return 0;
}
''', encoding="utf-8")
            target = _testbuild.NativeTarget(
                "npc-pose-jitter-check",
                [_testbuild.unit(cpp, std="c++17", warnings="strict", includes=(ROOT / "src",))],
            )
            executed = target.run(timeout=30)
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)


if __name__ == "__main__":
    unittest.main()
