// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-free policy for the client's automatic entry into the host's world. The host's reliable WorldHost note
// (new epoch on every load/new game/menu; kind Menu while loading, New/Saved once playable) is the load event; this
// decides WHEN the client may enter. It never touches the engine: persist.cpp builds the Snapshot, runs Step at the outer
// module boundary and performs the entry only through persist::EnterHostWorld (template LoadGame) or, before a template
// exists, the first native New Game.
#pragma once

#include <cstdint>

namespace g3t::autoload {
constexpr uint32_t CountdownMs = 4000;   // visible and cancelable; also coalesces rapid host epochs
constexpr uint32_t RecordWaitMs = 8000;  // wait for the account record (wording only) at most this long
constexpr uint32_t RetryMs[2] = {3000, 8000};
constexpr uint32_t AdmitStallMs = 60000; // entered, but the host has not admitted us
constexpr unsigned MaxEntries = 3;       // automatic entries per host epoch

enum class Action : uint8_t { None, Enter };
enum class Status : uint8_t {
    Off,         // config off, host role, or no native client hooks
    NotJoined,   // connecting/reconnecting; entry follows the join
    Refused,     // protocol/install mismatch: never enter
    HostLoading, // no host world or kind Menu: the client waits
    WaitRecord,  // the account record has not arrived yet (bounded)
    Busy,        // native load/save, nested frame, menu command or save exchange, or the menu is not ready
    NoRoute,     // neither the template route nor the first New Game is available (restart Gothic 3)
    InWorld,     // a live local world without a queued entry: never touched
    Countdown,
    Retry,
    Entering,    // approved: queued behind dialogue/trade/save/native work, or running
    Admitting,   // entered; waiting for the host's admission
    Stalled,     // entered; no admission for AdmitStallMs
    Idle,        // admitted to the host's current epoch
    Held,        // canceled for this epoch
    GaveUp,      // MaxEntries failed for this epoch
};

struct Snapshot {
    bool enabled = false;        // client_autoload = on, client role, native client hooks verified
    bool joined = false;
    bool refused = false;
    uint32_t epoch = 0;          // host world epoch last announced (0 none)
    bool hostPlayable = false;   // kind New/Saved
    bool localWorld = false;     // a player exists or the session runs
    bool queued = false;         // an automatic entry is queued (stale world frozen) and waits for approval
    bool admitted = false;       // own hero admitted to the current epoch
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

class Controller {
    struct Timer {
        bool on = false;
        uint32_t at = 0;
    };
    uint32_t epoch_ = 0;
    bool playable_ = false, held_ = false, entering_ = false, admitting_ = false, retryDue_ = false;
    Timer ready_, retry_, admit_, record_;
    unsigned entries_ = 0;

    static bool Reached(uint32_t now, uint32_t at) { return static_cast<int32_t>(now - at) >= 0; }
    void Restart() { ready_.on = false; }
    void NewEpoch(uint32_t epoch)
    {
        epoch_ = epoch;
        held_ = entering_ = admitting_ = retryDue_ = false;
        ready_ = retry_ = admit_ = record_ = {};
        entries_ = 0;
    }

  public:
    unsigned Entries() const { return entries_; }
    bool Held() const { return held_; }
    bool Entering() const { return entering_; }

    Decision Step(Snapshot const& s, uint32_t now)
    {
        if (!s.enabled) {
            NewEpoch(0);
            playable_ = false;
            return {Action::None, Status::Off, 0};
        }
        if (s.epoch != epoch_) NewEpoch(s.epoch);
        if (s.hostPlayable != playable_) { playable_ = s.hostPlayable; Restart(); record_.on = false; }
        if (!s.joined) { Restart(); return {Action::None, Status::NotJoined, 0}; }
        if (s.refused) { Restart(); return {Action::None, Status::Refused, 0}; }
        if (!s.epoch || !s.hostPlayable) {
            Restart();
            admitting_ = false;
            return {Action::None, Status::HostLoading, 0};
        }
        if (s.admitted) {
            Restart();
            entering_ = admitting_ = retryDue_ = false;
            entries_ = 0;
            return {Action::None, Status::Idle, 0};
        }
        if (held_) { Restart(); return {Action::None, Status::Held, 0}; }
        if (entering_) return {Action::None, Status::Entering, 0};
        if (admitting_) {
            if (!s.localWorld) admitting_ = false; // the entry was undone (back at the title screen): start over
            else if (Reached(now, admit_.at + AdmitStallMs)) return {Action::None, Status::Stalled, 0};
            else return {Action::None, Status::Admitting, 0};
        }
        if (s.localWorld && !s.queued) { Restart(); return {Action::None, Status::InWorld, 0}; }
        if (!s.routeAvailable) { Restart(); return {Action::None, Status::NoRoute, 0}; }
        if (s.busy || !s.entryReady) { Restart(); return {Action::None, Status::Busy, 0}; }
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
};
}
