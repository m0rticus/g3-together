// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-free policy for the client's admission into the host's world. The host's reliable WorldHost note (new epoch on every
// load/new game/menu; kind Menu while loading, New/Saved once playable) is the load event; this decides WHEN the client enters
// and, above all, that a connected client is never left playing silently in a world that is not the host's: a live local world
// that is not admitted and cannot become so by itself is detected (Mismatch) and corrected through the same cancelable
// countdown, and whenever correction is impossible (unsupported install, autoload off, Esc, no template, gave up) the status
// says so. It never touches the engine: persist.cpp builds the Snapshot, runs Step at the outer module boundary and performs
// the entry only through persist::EnterHostWorld (template LoadGame) or, before a template exists, the first native New Game.
#pragma once

#include <cstdint>

namespace g3t::autoload {
constexpr uint32_t CountdownMs = 4000;   // visible and cancelable; also coalesces rapid host epochs
constexpr uint32_t RecordWaitMs = 8000;  // wait for the account record (wording only) at most this long
constexpr uint32_t RetryMs[2] = {3000, 8000};
constexpr uint32_t AdmitStallMs = 60000; // entered, but the host has not admitted us
constexpr uint32_t AdoptStallMs = 30000; // a local world that may still be admitted by itself, but is not after this long
constexpr uint32_t JoinGraceMs = 8000;   // normal admission takes a few seconds; after this the player is told it is slow
constexpr uint32_t MismatchMs = 2000;    // a world that cannot be admitted must persist this long before the correction starts
constexpr unsigned MaxEntries = 3;       // automatic entries per host epoch

enum class Action : uint8_t { None, Enter };
enum class Status : uint8_t {
    Off,         // host role
    Unsupported, // this install cannot host-manage worlds (native hooks off): it can never be admitted
    NotJoined,   // connecting/reconnecting; entry follows the join
    Refused,     // protocol/install mismatch: never enter
    HostLoading, // no host world or kind Menu: the client waits
    ManualOnly,  // client_autoload = off: connected, not in the host's world, the player must choose New Game
    WaitRecord,  // the account record has not arrived yet (bounded)
    Busy,        // native load/save, nested frame, menu command or save exchange, or the menu is not ready
    NoRoute,     // neither the template route nor the first New Game is available (restart Gothic 3)
    Joining,     // a local world that is being admitted by the normal flow (fresh world / entry we made): a few seconds
    JoiningSlow, // same, but longer than JoinGraceMs: the player is told
    Mismatch,    // a local world that cannot be admitted by itself, debounced before the correction countdown
    Countdown,
    Retry,
    Entering,    // approved: queued behind dialogue/trade/save/native work, or running
    Admitting,   // entered; waiting for the host's admission
    Stalled,     // admission did not complete in time: the world is distrusted and corrected again (bounded)
    Idle,        // admitted to the host's current epoch
    Held,        // canceled for this epoch
    GaveUp,      // MaxEntries failed for this epoch
};

struct Snapshot {
    bool client = false;         // client role (the host has nothing to admit)
    bool hooks = false;          // native client hooks verified; false: the install is unsupported
    bool enabled = false;        // client_autoload = on (with client and hooks)
    bool joined = false;
    bool refused = false;
    uint32_t epoch = 0;          // host world epoch last announced (0 none)
    bool hostPlayable = false;   // kind New/Saved
    bool localWorld = false;     // a player exists or the session runs
    bool queued = false;         // an automatic entry is queued (stale world frozen) and waits for approval
    bool admitted = false;       // own hero admitted to the current epoch
    bool adoptable = false;      // the live local world can still be admitted by itself (fresh start world, or the admission of this epoch is in flight)
    bool busy = false;           // native work, nested frame or save exchange
    bool routeAvailable = false; // template entry valid, or the first New Game possible
    bool entryReady = false;     // title screen: main menu open; queued entry: nothing more to wait for
    bool recordKnown = false;    // this account's record (or "none") for the current epoch arrived
};

struct Decision {
    Action action = Action::None;
    Status status = Status::Off;
    uint32_t remainingMs = 0;
};

inline bool Cancelable(Status status) { return status == Status::Countdown || status == Status::Retry; }

// Statuses that give the player a lasting, specific message: a connected client in a world that is not the host's must always
// be in one of these, or in a bounded transient one (Joining, Mismatch, WaitRecord, Busy, Entering, Admitting), never silent.
inline bool Told(Status status)
{
    switch (status) {
    case Status::Unsupported: case Status::Refused: case Status::HostLoading: case Status::ManualOnly: case Status::NoRoute:
    case Status::JoiningSlow: case Status::Countdown: case Status::Retry: case Status::Stalled: case Status::Held:
    case Status::GaveUp:
        return true;
    default:
        return false;
    }
}
// A blocked state cannot fix itself: only the player (menu New Game, restart, config) or a new host epoch can.
inline bool Blocked(Status status)
{
    switch (status) {
    case Status::Unsupported: case Status::Refused: case Status::ManualOnly: case Status::NoRoute: case Status::Held:
    case Status::GaveUp:
        return true;
    default:
        return false;
    }
}
inline char const* Name(Status status)
{
    switch (status) {
    case Status::Off: return "off";
    case Status::Unsupported: return "unsupported";
    case Status::NotJoined: return "not-joined";
    case Status::Refused: return "refused";
    case Status::HostLoading: return "host-loading";
    case Status::ManualOnly: return "manual-only";
    case Status::WaitRecord: return "wait-record";
    case Status::Busy: return "busy";
    case Status::NoRoute: return "no-route";
    case Status::Joining: return "joining";
    case Status::JoiningSlow: return "joining-slow";
    case Status::Mismatch: return "mismatch";
    case Status::Countdown: return "countdown";
    case Status::Retry: return "retry";
    case Status::Entering: return "entering";
    case Status::Admitting: return "admitting";
    case Status::Stalled: return "stalled";
    case Status::Idle: return "idle";
    case Status::Held: return "held";
    case Status::GaveUp: return "gave-up";
    }
    return "?";
}

class Controller {
    struct Timer {
        bool on = false;
        uint32_t at = 0;
    };
    uint32_t epoch_ = 0;
    bool playable_ = false, held_ = false, entering_ = false, admitting_ = false, retryDue_ = false, distrust_ = false;
    Timer ready_, retry_, admit_, record_, adopt_, mismatch_;
    unsigned entries_ = 0;

    static bool Reached(uint32_t now, uint32_t at) { return static_cast<int32_t>(now - at) >= 0; }
    void Restart() { ready_.on = false; }
    void Forget() { adopt_.on = mismatch_.on = false; }
    // The admission clock only runs while connected to a playable host world: a world entered before the connection (or while the
    // host was away) must not look "stalled" the moment the connection arrives.
    void Pause(uint32_t now) { if (admitting_) admit_.at = now; }
    void NewEpoch(uint32_t epoch)
    {
        epoch_ = epoch;
        held_ = entering_ = admitting_ = retryDue_ = distrust_ = false;
        ready_ = retry_ = admit_ = record_ = adopt_ = mismatch_ = {};
        entries_ = 0;
    }

  public:
    unsigned Entries() const { return entries_; }
    bool Held() const { return held_; }
    bool Entering() const { return entering_; }

    Decision Step(Snapshot const& s, uint32_t now)
    {
        if (!s.client) {
            NewEpoch(0);
            playable_ = false;
            return {Action::None, Status::Off, 0};
        }
        if (!s.hooks) { // never admitted, never silent: the caller says why
            NewEpoch(0);
            playable_ = false;
            return {Action::None, Status::Unsupported, 0};
        }
        if (!s.enabled) { // client_autoload = off: nothing automatic, but the player is told what to do
            NewEpoch(0);
            playable_ = false;
            if (!s.joined) return {Action::None, Status::Off, 0};
            if (s.refused) return {Action::None, Status::Refused, 0};
            if (!s.epoch || !s.hostPlayable) return {Action::None, Status::HostLoading, 0};
            return {Action::None, s.admitted ? Status::Idle : Status::ManualOnly, 0};
        }
        if (s.epoch != epoch_) NewEpoch(s.epoch);
        if (s.hostPlayable != playable_) { playable_ = s.hostPlayable; Restart(); record_.on = false; }
        if (!s.joined) { Restart(); Forget(); Pause(now); return {Action::None, Status::NotJoined, 0}; }
        if (s.refused) { Restart(); Forget(); Pause(now); return {Action::None, Status::Refused, 0}; }
        if (!s.epoch || !s.hostPlayable) {
            Restart();
            Forget();
            admitting_ = false;
            return {Action::None, Status::HostLoading, 0};
        }
        if (s.admitted) {
            Restart();
            Forget();
            entering_ = admitting_ = retryDue_ = distrust_ = false;
            entries_ = 0;
            return {Action::None, Status::Idle, 0};
        }
        if (held_) { Restart(); Forget(); return {Action::None, Status::Held, 0}; }
        if (entering_) return {Action::None, Status::Entering, 0};
        if (admitting_) {
            if (!s.localWorld) admitting_ = false; // the entry was undone (back at the title screen): start over
            else {
                const uint32_t waited = now - admit_.at;
                if (Reached(now, admit_.at + AdmitStallMs)) {
                    admitting_ = false;
                    distrust_ = true; // the host did not admit this world in time: correct it again (bounded by MaxEntries)
                    return {Action::None, Status::Stalled, 0};
                }
                return {Action::None, waited < JoinGraceMs ? Status::Admitting : Status::JoiningSlow, 0};
            }
        }
        const bool liveWorld = s.localWorld && !s.queued;
        if (liveWorld) {
            if (s.adoptable && !distrust_) { // the normal flow may still admit this world: wait, but tell and bound it
                mismatch_.on = false;
                if (!adopt_.on) adopt_ = {true, now};
                const uint32_t waited = now - adopt_.at;
                if (waited < JoinGraceMs) return {Action::None, Status::Joining, AdoptStallMs - waited};
                if (waited < AdoptStallMs) return {Action::None, Status::JoiningSlow, AdoptStallMs - waited};
                distrust_ = true;
                return {Action::None, Status::Stalled, 0};
            }
            adopt_.on = false;
            if (!mismatch_.on) mismatch_ = {true, now};
            if (!Reached(now, mismatch_.at + MismatchMs)) { Restart(); return {Action::None, Status::Mismatch, 0}; }
        } else Forget();
        if (!s.routeAvailable) { Restart(); return {Action::None, Status::NoRoute, 0}; }
        if (s.busy || !(s.entryReady || s.queued || liveWorld)) { Restart(); return {Action::None, Status::Busy, 0}; }
        if (entries_ >= MaxEntries) return {Action::None, Status::GaveUp, 0};
        if (retryDue_) {
            if (!Reached(now, retry_.at)) return {Action::None, Status::Retry, retry_.at - now};
            entering_ = true; // a retry does not wait for another countdown
            return {Action::Enter, Status::Retry, 0};
        }
        if (!s.recordKnown) {
            if (!record_.on) record_ = {true, now};
            if (!Reached(now, record_.at + RecordWaitMs)) { Restart(); return {Action::None, Status::WaitRecord, 0}; }
        }
        if (!ready_.on) ready_ = {true, now};
        const uint32_t elapsed = now - ready_.at;
        if (elapsed < CountdownMs) return {Action::None, Status::Countdown, CountdownMs - elapsed};
        entering_ = true;
        return {Action::Enter, Status::Countdown, 0};
    }

    // Outcome of the entry that Step requested (may arrive frames later: the entry waits for a safe boundary).
    void Entered(bool ok, uint32_t now)
    {
        ++entries_;
        entering_ = false;
        ready_.on = false;
        distrust_ = false;
        Forget();
        if (ok) {
            admitting_ = true;
            admit_ = {true, now};
            retryDue_ = false;
            return;
        }
        retryDue_ = entries_ < MaxEntries;
        retry_ = {true, now + RetryMs[entries_ == 1 ? 0 : 1]};
    }
    // The user said no (Esc during the countdown, or the manual New Game): stay out for this host epoch.
    void Cancel()
    {
        held_ = true;
        retryDue_ = false;
        Restart();
    }
    // The player's own entry (manual New Game, first or template) succeeded after Cancel(): lift the hold and wait for the host's
    // admission like for an automatic entry, so a world that is never admitted is still corrected instead of left silently.
    void Adopted(uint32_t now)
    {
        held_ = entering_ = retryDue_ = distrust_ = false;
        admitting_ = true;
        admit_ = {true, now};
        ready_.on = false;
        Forget();
    }
};
}
