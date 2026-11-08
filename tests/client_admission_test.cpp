// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/client_autoload.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace g3t::autoload;

namespace {
constexpr uint32_t TickMs = 50;
constexpr uint32_t AdmitDelayMs = 2000;
constexpr uint32_t Never = 0xffffffffu;
constexpr uint32_t BoundMs = MismatchMs + AdoptStallMs + CountdownMs + RecordWaitMs + 2 * RetryMs[1] + 10000;

enum class World { None, Fresh, Used };

struct Watch {
    bool active = false;
    uint32_t since = 0, statusSince = 0, silentSince = 0;
    Status last = Status::Off;
    bool failed = false;
    const char* reason = nullptr;
    uint32_t when = 0, age = 0, mask = 0;
    Status status = Status::Off;

    void Fail(const char* why, uint32_t now, uint32_t elapsed, Status actual, uint32_t inputs)
    {
        if (failed) return;
        failed = true; reason = why; when = now; age = elapsed; status = actual; mask = inputs;
    }

    // Silence is bounded per run, not cumulatively: Mismatch (2 s) may follow a told Stalled, WaitRecord (8 s) may follow Mismatch.
    static constexpr uint32_t SilentRunMs = JoinGraceMs + MismatchMs + RecordWaitMs;

    void Check(Snapshot const& s, Decision d, uint32_t now, uint32_t inputs = 0)
    {
        const bool observed = s.client && s.joined && s.epoch &&
            s.hostPlayable && s.localWorld && !s.admitted;
        if (!observed) { active = false; return; }
        const bool silent = !Told(d.status) && d.status != Status::Entering;
        if (!active) {
            active = true; since = statusSince = silentSince = now; last = d.status;
        } else if (last != d.status) {
            statusSince = now; last = d.status;
        }
        if (!silent) silentSince = now;
        const uint32_t elapsed = now - since;
        const uint32_t statusElapsed = now - statusSince;
        struct Limit { Status status; uint32_t duration; };
        const Limit limits[] = {
            {Status::Joining, JoinGraceMs}, {Status::Mismatch, MismatchMs},
            {Status::WaitRecord, RecordWaitMs}, {Status::Admitting, JoinGraceMs},
            {Status::Entering, AdmitStallMs},
        };
        for (auto const& limit : limits) {
            if (d.status == limit.status && statusElapsed > limit.duration)
                Fail("transient status exceeded its own limit", now, elapsed, d.status, inputs);
        }
        if (silent && !(d.status == Status::Busy && s.busy) && now - silentSince > SilentRunMs)
            Fail("unadmitted world silent past one bounded silent run", now, elapsed, d.status, inputs);
    }

    void Finish(const char* name) const
    {
        if (failed)
            std::fprintf(stderr, "%s: %s: t=%u age=%u status=%s mask=%u\n",
                         name, reason, when, age, Name(status), mask);
        assert(!failed);
    }
};

struct Sim {
    Controller controller;
    Watch watch;
    uint32_t now = 0, epoch = 0, enteredEpoch = 0, admitAt = 0, delay = AdmitDelayMs;
    bool playable = false, joined = false, refused = false, hooks = true, enabled = true;
    bool route = true, busy = false, admitted = false, queued = false, admitTimer = false;
    bool templateAvailable = false, firstNewGame = true;
    World world = World::None;
    Decision last;
    unsigned enters = 0, stalls = 0, slow = 0;
    uint32_t lastEnterEpoch = 0;

    void Host(uint32_t newEpoch, bool newPlayable)
    {
        if (newEpoch != epoch) {
            queued = world != World::None && playable && admitted;
            if (world != World::None) world = World::Used;
            admitted = admitTimer = false;
            enteredEpoch = 0;
        }
        epoch = newEpoch;
        playable = newPlayable;
    }

    void Tick()
    {
        const bool candidate = joined && hooks && !refused && playable && epoch && !queued &&
            world != World::None && (world == World::Fresh || enteredEpoch == epoch);
        if (!candidate || admitted) admitTimer = false;
        else if (!admitTimer) { admitTimer = true; admitAt = now; }
        else if (delay != Never && now - admitAt >= delay) { admitted = true; admitTimer = false; }

        Snapshot s;
        s.client = true; s.hooks = hooks; s.enabled = enabled; s.joined = joined; s.refused = refused;
        s.epoch = epoch; s.hostPlayable = playable; s.localWorld = world != World::None;
        s.queued = queued; s.admitted = admitted;
        s.adoptable = world == World::Fresh || (enteredEpoch && enteredEpoch == epoch);
        s.busy = busy; s.routeAvailable = route && (templateAvailable || firstNewGame);
        s.entryReady = true; s.recordKnown = true;
        last = controller.Step(s, now);
        watch.Check(s, last, now);
        if (last.status == Status::Stalled) ++stalls;
        if (last.status == Status::JoiningSlow) ++slow;
        if (last.action == Action::Enter) {
            assert(joined && !refused && playable && epoch && hooks && enabled && !busy);
            assert(s.routeAvailable);
            ++enters; lastEnterEpoch = epoch;
            controller.Entered(true, now);
            world = World::Fresh; enteredEpoch = epoch; queued = admitted = admitTimer = false;
            templateAvailable = true;
        }
        now += TickMs;
    }

    void Run(uint32_t duration)
    {
        for (uint32_t t = 0; t < duration; t += TickMs) Tick();
    }

    void UntilAdmitted()
    {
        const uint32_t start = now;
        while (!admitted && now - start <= BoundMs) Tick();
        assert(admitted && now - start <= BoundMs);
    }
};

void HostFirst()
{
    Sim s; s.Host(1, true); s.Run(500); s.joined = true;
    assert(!s.templateAvailable && s.firstNewGame);
    s.UntilAdmitted();
    assert(s.enters == 1 && s.lastEnterEpoch == 1);
    s.watch.Finish("host-first");
}

void ClientFirst()
{
    Sim s; s.joined = true; s.Host(1, false); s.Run(1000);
    assert(s.last.status == Status::HostLoading && s.enters == 0);
    s.Host(2, true); s.UntilAdmitted();
    assert(s.enters == 1 && s.lastEnterEpoch == 2);
    s.watch.Finish("client-first");
}

void FreshAdoption()
{
    Sim s; s.world = World::Fresh; s.Host(1, true);
    s.world = World::Fresh; s.joined = true;
    s.UntilAdmitted();
    assert(s.enters == 0 && s.now <= AdmitDelayMs + TickMs);
    s.watch.Finish("fresh-adoption");
}

void AdmissionNeverCompletes()
{
    Sim s; s.Host(1, true); s.joined = true; s.world = World::Fresh; s.delay = Never;
    s.Run(250000);
    assert(s.slow && s.stalls == MaxEntries + 1);
    assert(s.enters == MaxEntries && s.last.status == Status::GaveUp && Told(s.last.status));
    s.watch.Finish("admission-never-completes");
}

void MenuToWorldMismatch()
{
    Sim s; s.Host(1, false); s.world = World::Used; s.joined = true; s.Run(500);
    s.Host(2, true); assert(!s.queued);
    s.Tick(); assert(s.last.status == Status::Mismatch);
    s.Run(MismatchMs - TickMs); s.Tick(); assert(s.last.status == Status::Countdown);
    assert(s.last.remainingMs == CountdownMs);
    s.UntilAdmitted(); assert(s.enters == 1);
    s.watch.Finish("menu-to-world-mismatch");
}

void CancelPerEpoch()
{
    Sim s; s.Host(1, true); s.joined = true; s.world = World::Used;
    s.Run(MismatchMs + TickMs);
    assert(s.last.status == Status::Countdown);
    s.controller.Cancel(); s.Run(100000);
    assert(s.last.status == Status::Held && Told(s.last.status) && !s.enters);
    s.Host(2, true); s.Tick(); assert(s.last.status == Status::Mismatch);
    s.UntilAdmitted(); assert(s.enters == 1 && s.lastEnterEpoch == 2);
    s.watch.Finish("cancel-per-epoch");
}

void AutoloadOff()
{
    Sim s; s.Host(1, true); s.joined = true; s.enabled = false; s.world = World::Used;
    s.Run(20000); assert(s.last.status == Status::ManualOnly && Told(s.last.status) && !s.enters);
    s.world = World::None; s.Tick(); assert(s.last.status == Status::ManualOnly);
    s.watch.Finish("autoload-off");
}

void MissingRoute()
{
    Sim s; s.Host(1, true); s.joined = true; s.world = World::Used;
    s.route = false; s.Run(20000);
    assert(s.last.status == Status::NoRoute && Told(s.last.status) && !s.enters);
    s.route = true; s.Tick(); assert(s.last.status == Status::Countdown);
    s.UntilAdmitted(); assert(s.enters == 1);
    s.watch.Finish("missing-route");
}

void Refused()
{
    Sim s; s.Host(1, true); s.joined = s.refused = true; s.world = World::Used;
    s.Run(100000); assert(s.last.status == Status::Refused && Told(s.last.status) && !s.enters);
    s.watch.Finish("refused");
}

void HooksOff()
{
    Sim s; s.Host(1, true); s.joined = true; s.hooks = false;
    for (World w : {World::None, World::Fresh, World::Used}) {
        s.world = w; s.admitted = false; s.Tick();
        assert(s.last.status == Status::Unsupported && Told(s.last.status) && !s.enters);
    }
    s.watch.Finish("hooks-off");
}

void HostMenu()
{
    Sim s; s.Host(1, false); s.joined = true; s.Run(10000);
    assert(s.last.status == Status::HostLoading && Told(s.last.status) && !s.enters);
    s.Host(2, true); s.Tick(); assert(s.last.status == Status::Countdown);
    s.UntilAdmitted(); assert(s.enters == 1);
    s.watch.Finish("host-menu");
}

void HostLoadsSave()
{
    Sim s; s.Host(1, true); s.joined = true; s.UntilAdmitted();
    s.Host(2, true); assert(s.queued && !s.admitted && s.world == World::Used);
    s.Tick(); assert(s.last.status == Status::Countdown);
    s.UntilAdmitted(); assert(s.enters == 2 && s.lastEnterEpoch == 2);
    s.watch.Finish("host-loads-save");
}

void RapidEpochs()
{
    Sim s; s.joined = true;
    for (uint32_t epoch = 1; epoch <= 6; ++epoch) {
        s.Host(epoch, epoch % 2 == 0);
        s.Run(700); assert(!s.enters);
    }
    s.UntilAdmitted();
    assert(s.enters == 1 && s.lastEnterEpoch == 6);
    s.watch.Finish("rapid-epochs");
}

void ClientRestart()
{
    Sim s; s.Host(9, true); s.joined = true;
    s.UntilAdmitted(); assert(s.enters == 1 && s.lastEnterEpoch == 9);
    s.watch.Finish("client-restart");
}

void BusyRestartsCountdown()
{
    Sim s; s.Host(1, true); s.joined = true;
    s.Run(2000); assert(s.last.status == Status::Countdown);
    s.busy = true; s.Run(10000); assert(s.last.status == Status::Busy && !s.enters);
    s.busy = false; s.Tick();
    assert(s.last.status == Status::Countdown && s.last.remainingMs == CountdownMs);
    s.Run(CountdownMs - TickMs); assert(!s.enters);
    s.UntilAdmitted(); assert(s.enters == 1);
    s.watch.Finish("busy-restarts-countdown");
}

void ManualNewGameIsAdoptedAndBounded()
{
    // The player's own New Game cancels the automatic entry (held); once it succeeded its admission is awaited, and a world the
    // host never admits is still corrected (countdown, Esc) instead of being left to play silently.
    Sim s; s.Host(1, true); s.joined = true; s.Run(1000);
    s.controller.Cancel(); s.Run(500); assert(s.last.status == Status::Held);
    s.world = World::Fresh; s.delay = Never; s.controller.Adopted(s.now);
    s.Tick(); assert(s.last.status == Status::Admitting);
    s.Run(JoinGraceMs); assert(s.last.status == Status::JoiningSlow && Told(s.last.status));
    s.Run(AdmitStallMs); assert(s.enters >= 1);
    s.watch.Finish("manual-new-game-adopted");
}

void SoloWorldThenConnect()
{
    // A New Game entered by hand before the connection arrives (host world already announced) and played for minutes: its admission
    // clock only starts with the connection, so the world is adopted, never "stalled" and corrected at the moment of joining.
    Sim s; s.Host(1, true); s.world = World::Fresh;
    s.Tick(); // the controller has seen the host's epoch (every frame does) before the player's New Game finishes
    s.controller.Adopted(s.now);
    s.Run(300000); assert(s.last.status == Status::NotJoined);
    s.joined = true; s.Tick(); assert(s.last.status == Status::Admitting);
    s.UntilAdmitted();
    assert(s.enters == 0 && s.stalls == 0);
    s.watch.Finish("solo-world-then-connect");
}

void ExhaustiveSweep()
{
    Watch firstFailure, firstBusyFailure;
    for (uint32_t mask = 0; mask < (1u << 13); ++mask) {
        Snapshot s;
        s.client = mask & 1; s.hooks = mask & 2; s.enabled = mask & 4; s.joined = mask & 8;
        s.refused = mask & 16; s.hostPlayable = mask & 32; s.localWorld = mask & 64; s.queued = mask & 128;
        s.admitted = mask & 256; s.adoptable = mask & 512; s.routeAvailable = mask & 1024;
        s.entryReady = mask & 2048; s.recordKnown = mask & 4096;
        for (uint32_t epoch : {0u, 9u}) {
            s.epoch = epoch;
            Controller c;
            Watch watch;
            bool entered = false, blocked = false;
            for (uint32_t now = 0; now <= 120000; now += TickMs) {
                const Decision d = c.Step(s, now);
                watch.Check(s, d, now, mask);
                blocked = blocked || Blocked(d.status);
                if (d.action == Action::Enter) { entered = true; c.Entered(true, now); }
            }
            const bool applicable = s.client && s.hooks && s.joined && !s.refused && s.epoch &&
                s.hostPlayable && s.localWorld && !s.admitted;
            if (applicable && !entered && !blocked)
                watch.Fail("neither entry nor blocked status within 120s", 120000, 120000, c.Step(s, 120000).status, mask);
            if (watch.failed && !firstFailure.failed) firstFailure = watch;
            if (watch.failed && watch.status == Status::Busy && !firstBusyFailure.failed) firstBusyFailure = watch;
        }
    }
    if (firstBusyFailure.failed)
        std::fprintf(stderr, "exhaustive-sweep: busy without busy input: t=%u age=%u mask=%u\n",
                     firstBusyFailure.when, firstBusyFailure.age, firstBusyFailure.mask);
    firstFailure.Finish("exhaustive-sweep");
}

struct Case { const char* name; void (*run)(); };
const Case cases[] = {
    {"host-first", HostFirst}, {"client-first", ClientFirst}, {"fresh-adoption", FreshAdoption},
    {"admission-never-completes", AdmissionNeverCompletes}, {"menu-to-world-mismatch", MenuToWorldMismatch},
    {"cancel-per-epoch", CancelPerEpoch}, {"autoload-off", AutoloadOff}, {"missing-route", MissingRoute},
    {"refused", Refused}, {"hooks-off", HooksOff}, {"host-menu", HostMenu}, {"host-loads-save", HostLoadsSave},
    {"rapid-epochs", RapidEpochs}, {"client-restart", ClientRestart}, {"busy-restarts-countdown", BusyRestartsCountdown},
    {"manual-new-game-adopted", ManualNewGameIsAdoptedAndBounded},
    {"solo-world-then-connect", SoloWorldThenConnect},
    {"exhaustive-sweep", ExhaustiveSweep},
};
}

int main(int argc, char** argv)
{
    assert(argc <= 2);
    bool found = argc == 1;
    for (auto const& test : cases) {
        if (argc == 1 || std::strcmp(argv[1], test.name) == 0) { found = true; test.run(); }
    }
    assert(found);
    return 0;
}
