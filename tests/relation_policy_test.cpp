#include "../src/relation_policy.h"
#include <cassert>

using namespace g3t::relation;

bool EqualText(char const* left, char const* right)
{
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

int main()
{
    // Name colours, party membership, death, hostile tags, and colour names.
    {
        Colour const colours[] = {
            Colour::Cream, Colour::Green, Colour::Tan, Colour::Orange,
            Colour::Red, Colour::Red, Colour::OrangeRed
        };
        (void)colours;
        for (int attitude = None; attitude <= Panic; ++attitude) {
            (void)attitude;
            assert(NameColour(attitude, false, false) == colours[attitude]);
            assert(NameColour(attitude, false, true) ==
                   (attitude == Friendly ? Colour::Blue : colours[attitude]));
            assert(NameColour(attitude, true, false) == Colour::Tan);
            assert(NameColour(attitude, true, true) == Colour::Tan);
            assert(HostileTag(attitude, false) == (attitude == Hostile || attitude == Predator));
            assert(!HostileTag(attitude, true));
        }
        int const invalidAttitudes[] = {-1, 7, 255};
        for (int attitude : invalidAttitudes) {
            (void)attitude;
            assert(NameColour(attitude, false, false) == Colour::Tan);
            assert(NameColour(attitude, false, true) == Colour::Tan);
            assert(NameColour(attitude, true, false) == Colour::Tan);
            assert(NameColour(attitude, true, true) == Colour::Tan);
            assert(!HostileTag(attitude, false));
            assert(!HostileTag(attitude, true));
        }
        assert(EqualText(ColourName(Colour::Cream), "cream"));
        assert(EqualText(ColourName(Colour::Green), "green"));
        assert(EqualText(ColourName(Colour::Blue), "blue"));
        assert(EqualText(ColourName(Colour::Tan), "tan"));
        assert(EqualText(ColourName(Colour::Orange), "orange"));
        assert(EqualText(ColourName(Colour::Red), "red"));
        assert(EqualText(ColourName(Colour::OrangeRed), "orange-red"));
        assert(EqualText(ColourName(static_cast<Colour>(0xFF)), "?"));
    }

    // Knock-down and revive never change name-writer truth; targeting is refused only while unavailable.
    {
        int const truths[] = {Hostile, Neutral, Friendly, Predator};
        for (int truth : truths) {
            (void)truth;
            for (int unavailable = 0; unavailable <= 1; ++unavailable) {
                bool const playerUnavailable = unavailable != 0;
                (void)playerUnavailable;
                assert(!RefuseDownedAttitude(true, playerUnavailable));
                assert(StoredByWriter(truth, playerUnavailable, true) == truth);
                assert(RefuseDownedAttitude(false, playerUnavailable) == playerUnavailable);
                assert(StoredByWriter(truth, playerUnavailable, false) ==
                       (playerUnavailable ? None : truth));
            }
            bool const episode[] = {false, true, true, false, false};
            int stored = None;
            for (bool playerUnavailable : episode) {
                stored = StoredByWriter(truth, playerUnavailable, true);
                assert(stored == truth);
            }
            (void)stored;
        }
    }

    // A scoped name query uses the local player unless a remote player leads the NPC.
    {
        assert(FocusNameContext(true, false) == Context::LocalPlayer);
        assert(FocusNameContext(true, true) == Context::AsAsked);
        assert(FocusNameContext(false, false) == Context::AsAsked);
        assert(FocusNameContext(false, true) == Context::AsAsked);
    }

    // Healing applies only to live, unevaluated storage with valid nonzero truth.
    {
        for (int truth = Friendly; truth <= Panic; ++truth) {
            (void)truth;
            assert(HealStored(None, truth, true));
            assert(!HealStored(None, truth, false));
            for (int stored = Friendly; stored <= Panic; ++stored) {
                (void)stored;
                assert(!HealStored(stored, truth, true));
                assert(!HealStored(stored, truth, false));
            }
        }
        int const invalidTruths[] = {None, 7, -1};
        for (int truth : invalidTruths) {
            (void)truth;
            assert(!HealStored(None, truth, true));
            assert(!HealStored(None, truth, false));
        }
    }

    // Host application distinguishes unchanged, invalid, writable, and native-modified values.
    {
        assert(ApplyHost(Hostile, Hostile, Unset) == Apply::Keep);
        assert(ApplyHost(Neutral, 7, Unset) == Apply::Ignore);
        assert(ApplyHost(7, 7, Unset) == Apply::Ignore);
        assert(ApplyHost(Neutral, 0xFF, Unset) == Apply::Ignore);
        assert(ApplyHost(0xFF, 0xFF, Unset) == Apply::Ignore);
        assert(ApplyHost(Neutral, Hostile, Unset) == Apply::Write);
        assert(ApplyHost(Neutral, Hostile, Neutral) == Apply::Write);
        assert(ApplyHost(Friendly, Hostile, Neutral) == Apply::Repair);
    }

    // Range re-entry writes the host state, then repairs a native change and remains idempotent.
    {
        int stored = None;
        uint8_t lastWritten = Unset;
        uint8_t const wanted = Hostile;
        Apply action = ApplyHost(stored, wanted, lastWritten);
        assert(action == Apply::Write);
        if (action == Apply::Write || action == Apply::Repair) {
            stored = wanted;
            lastWritten = wanted;
        }
        assert(ApplyHost(stored, wanted, lastWritten) == Apply::Keep);
        stored = Neutral;
        action = ApplyHost(stored, wanted, lastWritten);
        assert(action == Apply::Repair);
        if (action == Apply::Write || action == Apply::Repair) {
            stored = wanted;
            lastWritten = wanted;
        }
        assert(stored == Hostile);
        assert(ApplyHost(stored, wanted, lastWritten) == Apply::Keep);
    }

    // Lease hand-back heals an imported zero but preserves every nonzero stale stored value.
    {
        assert(HealStored(None, Hostile, true));
        for (int stored = Friendly; stored <= Panic; ++stored) {
            (void)stored;
            assert(!HealStored(stored, Hostile, true));
        }
    }

    // A mirror answers from the host only when all lease and attitude conditions hold.
    {
        for (int mirrored = 0; mirrored <= 1; ++mirrored) {
            for (int currentSession = 0; currentSession <= 1; ++currentSession) {
                for (int haveRecord = 0; haveRecord <= 1; ++haveRecord) {
                    for (int attitude = None; attitude <= 7; ++attitude) {
                        for (int sharedValid = 0; sharedValid <= 1; ++sharedValid) {
                            bool const expected = mirrored != 0 && currentSession != 0 &&
                                                  haveRecord != 0 && sharedValid != 0 && attitude <= Panic;
                            (void)expected;
                            assert(MirrorAnswersFromHost(mirrored != 0, currentSession != 0,
                                                         haveRecord != 0, static_cast<uint8_t>(attitude),
                                                         sharedValid != 0) == expected);
                        }
                    }
                }
            }
        }
        assert(!MirrorAnswersFromHost(true, true, true, 0xFF));
        assert(!MirrorAnswersFromHost(true, true, true, Hostile, false));
    }

    // Client diagnostics separate drift, unapplied host updates, and normal playback delay.
    {
        Observation observation;
        (void)observation;
        observation.mirrorBound = true;
        observation.stored = Hostile;
        observation.applied = Hostile;
        observation.hostAttitude = Hostile;
        observation.shown = Hostile;
        assert(Mismatches(observation) == 0u);

        observation.stored = Neutral;
        observation.hostAttitude = Neutral;
        observation.shown = Neutral;
        assert(Mismatches(observation) == Drift);

        observation.applied = Neutral;
        observation.hostAttitude = Hostile;
        observation.shown = Friendly;
        assert(Mismatches(observation) == Unapplied);

        observation.shown = Neutral;
        assert(Mismatches(observation) == 0u);
    }

    // Host diagnostics respect life, the unsent age threshold, and remote leadership.
    {
        Observation observation;
        (void)observation;
        observation.stored = None;
        observation.truth = Hostile;
        assert(Mismatches(observation) == StaleNone);
        observation.alive = false;
        assert(Mismatches(observation) == 0u);

        observation.alive = true;
        observation.stored = Neutral;
        observation.truth = Neutral;
        observation.sent = Hostile;
        observation.sentAgeMs = 999;
        assert(Mismatches(observation) == 0u);
        observation.sentAgeMs = 1000;
        assert(Mismatches(observation) == Unsent);

        observation.sent = -1;
        observation.truth = Hostile;
        observation.ledByRemote = true;
        assert(Mismatches(observation) == 0u);
        observation.ledByRemote = false;
        assert(Mismatches(observation) == TruthDiffers);
        observation.stored = Hostile;
        assert(Mismatches(observation) == 0u);
    }

    // Deadlines remain ordered across 32-bit wraparound and policy constants stay stable.
    {
        uint32_t const beforeWrap = 0xFFFFFFF0u;
        uint32_t const nextAt = beforeWrap + 100u;
        (void)beforeWrap;
        (void)nextAt;
        assert(!Due(beforeWrap, nextAt));
        assert(!Due(nextAt - 1u, nextAt));
        assert(Due(nextAt, nextAt));
        assert(Due(nextAt + 1u, nextAt));
        assert(Due(0u, beforeWrap));
        assert(!Due(beforeWrap, 0u));
        // Unscheduled (0) is due at once even where the clock reads negative as int32 (uptime past 24.8 days).
        assert(DueOrUnset(0x90000000u, 0u));
        assert(DueOrUnset(5u, 0u));
        assert(!DueOrUnset(beforeWrap, nextAt));
        assert(DueOrUnset(nextAt, nextAt));
        assert(HealIntervalMs == 1500u);
        assert(DiagnosticIntervalMs == 5000u);
        assert(NearestCount == 6u);
        assert(Unset == 0xFFu);
    }
    return 0;
}
