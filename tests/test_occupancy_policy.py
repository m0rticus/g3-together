from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild


def source(name):
    return _testbuild.src_text(name)


def between(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


OCCUPANCY_POLICY_CODE = r"""
#undef NDEBUG
#include <cassert>
#include <initializer_list>
#include "occupancy_policy.h"

int main()
{
    using namespace g3t::occupancy_policy;

    // Non-interactable objects, empty users, and non-character users (e.g. Wth_Hailstorm) are untouched.
    {
        InteractUserState s;
        s.hasUser = true;
        s.userIsCharacter = true;
        s.userPassive = true;
        assert(!ShouldClearInteractUser(s));

        s.validObject = true;
        s.hasUser = false;
        assert(!ShouldClearInteractUser(s));

        s.hasUser = true;
        s.userIsCharacter = false;
        assert(!ShouldClearInteractUser(s));
    }

    // The local hero's active occupancy is never cleared.
    {
        InteractUserState s;
        s.validObject = true;
        s.hasUser = true;
        s.userIsCharacter = true;
        s.userIsLocalHero = true;
        s.userDeadOrDown = true;
        s.userPassive = true;
        assert(!ShouldClearInteractUser(s));
    }

    // Remote player puppets stay while marked in occupancy, and clear if unmarked.
    {
        InteractUserState s;
        s.validObject = true;
        s.hasUser = true;
        s.userIsCharacter = true;
        s.puppetOwner = 2;
        s.markedPlayer = 2;
        s.userPassive = true;
        assert(!ShouldClearInteractUser(s));
        assert(!ShouldDecrementAnchorUserCount(s));

        s.markedPlayer = 0;
        assert(ShouldClearInteractUser(s));
        // Unmarked puppets never incremented gCAnchor_PS::UserCount, so clearing them must not decrement it.
        assert(!ShouldDecrementAnchorUserCount(s));

        s.markedPlayer = 3;
        assert(ShouldClearInteractUser(s));
        assert(!ShouldDecrementAnchorUserCount(s));
    }

    // Silenced/passive, dead/down, suppressed, or combat/flee/abandoned NPCs retain stale User and are cleared,
    // and DO decrement gCAnchor_PS::UserCount.
    {
        InteractUserState live;
        live.validObject = true;
        live.hasUser = true;
        live.userIsCharacter = true;
        assert(!ShouldClearInteractUser(live));
        assert(!ShouldDecrementAnchorUserCount(live));

        for (bool* flag : {&live.userPassive, &live.userDeadOrDown, &live.userSuppressed, &live.userInactiveRoutine}) {
            *flag = true;
            assert(ShouldClearInteractUser(live));
            assert(ShouldDecrementAnchorUserCount(live));
            *flag = false;
        }
    }

    return 0;
}
"""

OCCUPANCY_TARGET = _testbuild.register_target(
    "occupancy_policy",
    [_testbuild.inline_unit("occupancy_policy_test.cpp", OCCUPANCY_POLICY_CODE)],
    msvc_vcvars=False,
)


class OccupancyPolicyTests(unittest.TestCase):
    def test_stale_interact_user_policy(self):
        result = OCCUPANCY_TARGET.run(timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_occupancy_and_focus_wiring_contract(self):
        occ = source("occupancy.cpp")
        self.assertIn("IsWorldInteractable(", occ)
        self.assertIn("eEPropertySetType_Item", occ)
        self.assertIn("gCInteractionAdmin::GetInstance().GetInteractionsInROI()", occ)
        self.assertIn("eEPropertySetType_Anchor", occ)
        self.assertIn("if (count > 0) anchorPs->SetUserCount(count - 1);", occ)
        self.assertIn("ScrubStaleUsers();", occ)
        self.assertIn('ClearStaleUser(instance, "remote-takeover")', occ)
        self.assertIn("occupancy_policy::ShouldClearInteractUser(", occ)
        self.assertIn("occupancy_policy::ShouldDecrementAnchorUserCount(", occ)

        playback = source("npcplayback.cpp")
        silence = between(playback, "void Silence(eCEntity* entity)", "void Stats(LogFn log)")
        self.assertIn("occupancy::ReleaseNpc(entity, true);", silence)

        ch = source("combat_hostility.cpp")
        filt = between(ch, "GEInt GE_STDCALL FilterFocusEntityPriority(", "void InstallHooks()")
        self.assertLess(
            filt.index('occupancy::ClearStaleUser(candidate, "focus")'),
            filt.rindex("originalFocusPriority(spu, self, other, args)"),
        )
        self.assertNotIn("InteractFocusPriority", source("relation_policy.h"))

    def test_touched_src_files_under_1000_lines_and_unthrottled(self):
        for name in (
            "occupancy.h", "occupancy.cpp", "occupancy_policy.h",
            "npcplayback.cpp", "combat_hostility.h", "combat_hostility.cpp", "relation_policy.h",
        ):
            text = source(name)
            lines = len(text.splitlines())
            self.assertLess(lines, 1000, f"{name} has {lines} lines (must be < 1000)")
            self.assertNotIn("RateLimit", text, name)
            self.assertNotIn("Throttle", text, name)


if __name__ == "__main__":
    unittest.main()
