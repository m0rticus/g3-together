// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/client_template.h"
#include "client_template_fixture.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>

using namespace g3t::rejoin::tmpl;

namespace {
Seal Sample()
{
    Seal s;
    s.game = 0x100000123ull;
    s.protocol = 42;
    s.headerSize = MinHeaderBytes;
    s.worldSize = MinWorldBytes;
    s.headerCrc = 0x0123abcdu;
    s.worldCrc = 0xdeadbeefu;
    return s;
}

void Marker()
{
    for (Origin origin : {Origin::NewGame, Origin::World}) {
        Seal s = Sample();
        s.origin = origin;
        const std::string text = Encode(s);
        Seal decoded;
        assert(Decode(text, decoded) == Parse::Ok);
        assert(decoded.version == s.version && decoded.game == s.game && decoded.protocol == s.protocol);
        assert(decoded.origin == origin && decoded.headerSize == s.headerSize && decoded.worldSize == s.worldSize);
        assert(decoded.headerCrc == s.headerCrc && decoded.worldCrc == s.worldCrc);
        for (size_t i = 0; i < text.size(); ++i)
            assert(Decode(text.substr(0, i), decoded) == (i == 0 ? Parse::Empty : Parse::Format));
        assert(Decode(text + "\n", decoded) == Parse::Format);
        assert(Decode(text + "extra=value\n", decoded) == Parse::Format);
        std::string bad = text;
        const size_t game = bad.find("game=");
        const size_t protocol = bad.find("protocol=");
        const size_t originAt = bad.find("origin=");
        bad.replace(game, originAt - game, text.substr(protocol, originAt - protocol) + text.substr(game, protocol - game));
        assert(Decode(bad, decoded) == Parse::Format);
        bad = text;
        bad[bad.find("crc=") + 4] = 'z';
        assert(Decode(bad, decoded) == Parse::Format);
        bad = text;
        bad.erase(bad.find("crc=") + 4, 1);
        assert(Decode(bad, decoded) == Parse::Format);
        bad = text;
        bad.pop_back();
        assert(Decode(bad, decoded) == Parse::Format);
        s.version = MarkerVersion - 1;
        assert(Decode(Encode(s), decoded) == Parse::Version);
        s.version = MarkerVersion + 1;
        assert(Decode(Encode(s), decoded) == Parse::Version);
    }
    Seal decoded;
    assert(Decode(std::string{}, decoded) == Parse::Empty);
    assert(Decode(std::string("garbage\n"), decoded) == Parse::Format);
    assert(Decode(std::string("G3T client template nope\n"), decoded) == Parse::Format);
}

void Structure()
{
    const auto header = template_fixture::Header(65 * 1024);
    assert(HeaderStructure(header) == Why::None);
    assert(HeaderStructure(template_fixture::Header(10, 0x00b0)) == Why::None);
    for (size_t size = 0; size < 10; ++size)
        assert(HeaderStructure(template_fixture::Bytes(header.begin(), header.begin() + size)) == Why::HeaderFormat);
    for (size_t i = 0; i < 8; ++i) {
        auto bad = header;
        bad[i] ^= 1;
        assert(HeaderStructure(bad) == Why::HeaderFormat);
    }
    for (uint16_t token : {uint16_t(0x00ae), uint16_t(0x00b1), uint16_t(0x01af)})
        assert(HeaderStructure(template_fixture::Header(10, token)) == Why::HeaderFormat);

    const auto world = template_fixture::World(2 * 1024 * 1024);
    assert(WorldStructure(world) == Why::None);
    assert(WorldStructure(template_fixture::World(17)) == Why::None);
    for (size_t size = 0; size < 17; ++size)
        assert(WorldStructure(template_fixture::Bytes(world.begin(), world.begin() + size)) == Why::WorldFormat);
    for (size_t i = 0; i < 8; ++i) {
        auto bad = world;
        bad[i] ^= 1;
        assert(WorldStructure(bad) == Why::WorldFormat);
    }
    auto bad = world;
    template_fixture::Word(bad, 8, 0);
    assert(WorldStructure(bad) == Why::WorldFormat);
    for (uint32_t length : {0u, static_cast<uint32_t>(world.size() - 15), UINT32_MAX}) {
        bad = world;
        template_fixture::Word(bad, 12, length);
        assert(WorldStructure(bad) == Why::WorldFormat);
    }
}

void Expect(Report const& r, Health health, Why why)
{
    assert(r.health == health && r.why == why);
}

void Inspection()
{
    const Seal seal = Sample();
    const Identity id{seal.game, seal.protocol};
    Facts f;
    Expect(Inspect(f, Parse::Empty, seal, id), Health::Missing, Why::NoFiles);
    f.header.exists = f.world.exists = f.marker.exists = true;
    f.header.size = seal.headerSize;
    f.world.size = seal.worldSize;
    auto inspect = [&](Parse p = Parse::Ok) { return Inspect(f, p, seal, id); };
    const Report valid = inspect();
    Expect(valid, Health::Valid, Why::None);
    assert(valid.sealed && valid.seal.game == seal.game);
    f.header.exists = false;
    Expect(inspect(), Health::Half, Why::NoHeader);
    f.header.exists = true; f.world.exists = false;
    Expect(inspect(), Health::Half, Why::NoWorld);
    f.world.exists = true; f.marker.exists = false;
    Expect(inspect(), Health::Half, Why::NoMarker);
    f.marker.exists = true;
    Expect(inspect(Parse::Format), Health::Corrupt, Why::MarkerFormat);
    Expect(inspect(Parse::Empty), Health::Corrupt, Why::MarkerFormat);
    Expect(inspect(Parse::Version), Health::Stale, Why::MarkerVersion);
    Expect(Inspect(f, Parse::Ok, seal, {0, id.protocol}), Health::Stale, Why::GameUnknown);
    Expect(Inspect(f, Parse::Ok, seal, {id.game + 1, id.protocol}), Health::Stale, Why::GameChanged);
    Expect(Inspect(f, Parse::Ok, seal, {id.game, static_cast<uint16_t>(id.protocol + 1)}), Health::Stale, Why::ProtocolChanged);
    f.header.size = 0; Expect(inspect(), Health::Corrupt, Why::HeaderEmpty);
    f.header.size = MinHeaderBytes - 1; Expect(inspect(), Health::Corrupt, Why::HeaderSize);
    f.header.size = MaxHeaderBytes + 1; Expect(inspect(), Health::Corrupt, Why::HeaderSize);
    f.header.size = seal.headerSize;
    f.world.size = 0; Expect(inspect(), Health::Corrupt, Why::WorldEmpty);
    f.world.size = MinWorldBytes - 1; Expect(inspect(), Health::Corrupt, Why::WorldSize);
    f.world.size = MaxWorldBytes + 1; Expect(inspect(), Health::Corrupt, Why::WorldSize);
    f.world.size = seal.worldSize + 1; Expect(inspect(), Health::Corrupt, Why::SizeMismatch);
    f.world.size = seal.worldSize; f.header.size = seal.headerSize + 1;
    Expect(inspect(), Health::Corrupt, Why::SizeMismatch);
    f.header.size = seal.headerSize;
    Expect(Verify(inspect(), seal.headerCrc, seal.worldCrc), Health::Valid, Why::None);
    Expect(Verify(inspect(), seal.headerCrc ^ 1, seal.worldCrc), Health::Corrupt, Why::CrcMismatch);
    Expect(Verify(inspect(), seal.headerCrc, seal.worldCrc ^ 1), Health::Corrupt, Why::CrcMismatch);
    Expect(Verify(Inspect({}, Parse::Empty, seal, id), 0, 0), Health::Missing, Why::NoFiles);
    const uint64_t signature = Signature(f);
    for (File* file : {&f.header, &f.world, &f.marker}) {
        file->exists = false; assert(Signature(f) != signature); file->exists = true;
        ++file->size; assert(Signature(f) != signature); --file->size;
        ++file->stamp; assert(Signature(f) != signature); --file->stamp;
    }
    assert(Signature(f) == signature);
}

void Publication()
{
    static_assert(SettleMs == 8000 && RecoverStableMs == 500);
    Publish p;
    assert(!p.Active() && p.phase() == Phase::Idle && !p.PollDue(0));
    const uint32_t t = 0xfffffff0u;
    p.Begin(Origin::World, t);
    assert(p.Active() && p.origin() == Origin::World && p.failure() == Fail::None);
    p.NativeReturned(true, 1, true, t);
    assert(p.PollDue(t));
    Facts f;
    f.header.exists = true; f.header.size = MinHeaderBytes;
    f.worldTmp.exists = f.worldTmp.locked = true;
    assert(p.Poll(f, t) == Step::Wait);
    assert(!p.PollDue(t + PollMs - 1) && p.PollDue(t + PollMs));
    assert(p.Poll(f, t + 300) == Step::Wait); // engine still owns the world .tmp
    f.worldTmp = {};
    f.world.exists = true; f.world.size = MinWorldBytes;
    assert(p.Poll(f, t + 350) == Step::Seal && p.Active());
    p.Finish(true);
    assert(!p.Active() && p.phase() == Phase::Done && p.failure() == Fail::None);
    assert(p.Poll(f, t + 400) == Step::Failed && !p.PollDue(t + 400));

    p.Begin(Origin::NewGame, t); p.NativeReturned(true, 1, true, t);
    assert(p.Poll({}, t + SettleMs - 1) == Step::Wait);
    assert(p.Poll({}, t + SettleMs) == Step::Failed && p.failure() == Fail::NoHeader && !p.Active());
    p.Begin(Origin::NewGame, t); p.NativeReturned(true, 1, true, t);
    f.world = {}; f.worldTmp.exists = f.worldTmp.locked = true;
    assert(p.Poll(f, t + SettleMs) == Step::Failed && p.failure() == Fail::Busy);
    for (bool header : {false, true}) {
        p.Begin(Origin::NewGame, t); p.NativeReturned(true, 1, true, t);
        Facts staging;
        staging.header.exists = !header; staging.header.size = MinHeaderBytes;
        staging.world.exists = header; staging.world.size = MinWorldBytes;
        File& tmp = header ? staging.headerTmp : staging.worldTmp;
        tmp.exists = true; tmp.size = header ? MinHeaderBytes : MinWorldBytes;
        const Step recover = header ? Step::RecoverHeader : Step::RecoverWorld;
        assert(p.Poll(staging, t + RecoverAfterMs - 1) == Step::Wait);
        assert(p.Poll(staging, t + RecoverAfterMs) == Step::Wait);
        assert(p.Poll(staging, t + RecoverAfterMs + RecoverStableMs - 1) == Step::Wait);
        assert(p.Poll(staging, t + RecoverAfterMs + RecoverStableMs) == recover);
        p.Begin(Origin::NewGame, t); p.NativeReturned(true, 1, true, t);
        assert(p.Poll(staging, t + RecoverAfterMs) == Step::Wait); // Begin clears the old candidate
        tmp.locked = true;
        assert(p.Poll(staging, t + RecoverAfterMs + 1) == Step::Wait); // a lock resets the timer
        tmp.locked = false;
        assert(p.Poll(staging, t + RecoverAfterMs + 2) == Step::Wait);
        assert(p.Poll(staging, t + RecoverAfterMs + 2 + RecoverStableMs - 1) == Step::Wait);
        assert(p.Poll(staging, t + RecoverAfterMs + 2 + RecoverStableMs) == recover);
        tmp.exists = false;
        assert(p.Poll(staging, t + RecoverAfterMs + 3 + RecoverStableMs) == Step::Wait);
        tmp.exists = true;
        assert(p.Poll(staging, t + RecoverAfterMs + 4 + RecoverStableMs) == Step::Wait);
        assert(p.Poll(staging, t + RecoverAfterMs + 4 + 2 * RecoverStableMs - 1) == Step::Wait);
        assert(p.Poll(staging, t + RecoverAfterMs + 4 + 2 * RecoverStableMs) == recover);
        assert(p.Poll(staging, t + SettleMs - 1) == recover);
        assert(p.Active());
        assert(p.Poll(staging, t + SettleMs) == Step::Failed);
        assert(p.failure() == (header ? Fail::NoHeader : Fail::NoWorld) && !p.Active());
    }
    p.Begin(Origin::NewGame, t); p.NativeReturned(true, 1, true, t);
    Facts complete;
    complete.header.exists = complete.world.exists = true;
    complete.header.size = MinHeaderBytes; complete.world.size = MinWorldBytes;
    assert(p.Poll(complete, t + SettleMs) == Step::Seal && p.Active());
    p.Begin(Origin::NewGame, t); p.NativeReturned(true, 1, true, t);
    Facts folder; folder.header.folderMissing = true;
    assert(p.Poll(folder, t + SettleMs) == Step::Failed && p.failure() == Fail::Folder);
    for (unsigned calls : {0u, 2u}) {
        p.Begin(Origin::NewGame, t); p.NativeReturned(true, calls, true, t);
        assert(!p.Active() && p.failure() == Fail::NativeCalls);
    }
    p.Begin(Origin::NewGame, t); p.NativeReturned(true, 1, false, t);
    assert(!p.Active() && p.failure() == Fail::NativeCalls);
    p.Begin(Origin::NewGame, t); p.NativeReturned(false, 0, false, t);
    assert(!p.Active() && p.failure() == Fail::NativeRefused);
    p.Begin(Origin::NewGame, t); p.Abort(Fail::Abandoned);
    assert(!p.Active() && p.failure() == Fail::Abandoned);
    p.Begin(Origin::NewGame, t); p.Finish(false, Fail::Cleanup);
    assert(!p.Active() && p.failure() == Fail::Cleanup);
}

void RecoveryCornerCases()
{
    Publish wrap;
    const uint32_t start = 0u - RecoverAfterMs;
    wrap.Begin(Origin::World, start);
    wrap.NativeReturned(true, 1, true, start);
    Facts f;
    f.header.exists = true; f.header.size = MinHeaderBytes;
    f.worldTmp.exists = true; f.worldTmp.size = MinWorldBytes;
    const Step atZero = wrap.Poll(f, 0);
    const Step beforeStable = wrap.Poll(f, RecoverStableMs - 1);
    const Step atStable = wrap.Poll(f, RecoverStableMs);
    const bool wrapOk = atZero == Step::Wait && beforeStable == Step::Wait && atStable == Step::RecoverWorld;
    if (!wrapOk) std::cerr << "candidate timer mishandles GetTickCount wrapping to zero\n";

    Publish switched;
    switched.Begin(Origin::World, 100);
    switched.NativeReturned(true, 1, true, 100);
    Facts different;
    different.headerTmp.exists = true; different.headerTmp.size = MinHeaderBytes;
    assert(switched.Poll(different, 100 + RecoverAfterMs) == Step::Wait);
    different.headerTmp.exists = false;
    different.worldTmp.exists = true; different.worldTmp.size = MinWorldBytes;
    const uint32_t changed = 100 + RecoverAfterMs + RecoverStableMs;
    const Step atChange = switched.Poll(different, changed);
    const Step beforeNewStable = switched.Poll(different, changed + RecoverStableMs - 1);
    const Step atNewStable = switched.Poll(different, changed + RecoverStableMs);
    const bool switchedOk = atChange == Step::Wait && beforeNewStable == Step::Wait && atNewStable == Step::RecoverWorld;
    if (!switchedOk) std::cerr << "a different recovery candidate inherited the previous candidate's timer\n";
    assert(wrapOk && switchedOk);
}

void Scenario(Situation const& s, Route route, Block block)
{
    const Plan p = Decide(s);
    assert(p.route == route && p.why == block);
}

void Decisions()
{
    Situation s;
    Scenario(s, Route::FirstEntry, Block::None); // title screen
    s.worldEntered = s.worldLoaded = s.worldRunning = s.quiet = true;
    Scenario(s, Route::Create, Block::None); // host load in a running world
    s.newAccount = true;
    Scenario(s, Route::Impossible, Block::CarriedHero); // deleted template, new account
    s.recordKnown = false;
    Scenario(s, Route::Wait, Block::Record);
    s = {}; s.health = Health::Stale;
    Scenario(s, Route::FirstEntry, Block::None); // game updated
    s.health = Health::Valid; s.origin = Origin::World;
    Scenario(s, Route::FirstEntry, Block::None); // existing account still gets a fresh hero
    s.recordKnown = false; s.newAccount = true;
    Scenario(s, Route::FirstEntry, Block::None); // fresh launch does not need the record yet
    s.worldEntered = s.worldLoaded = s.worldRunning = true;
    Scenario(s, Route::Wait, Block::Record); // carried template cannot enter an unknown account
    s.newAccount = false;
    Scenario(s, Route::Wait, Block::Record); // the unknown newAccount bit is ignored
    s.recordKnown = true;
    Scenario(s, Route::Template, Block::None);
    s.newAccount = true;
    Scenario(s, Route::Impossible, Block::CarriedHero);
    s.origin = Origin::NewGame; s.recordKnown = false;
    Scenario(s, Route::Template, Block::None);
    s = {}; s.worldEntered = s.worldLoaded = true;
    Scenario(s, Route::Wait, Block::NotQuiet); // hero loaded, session menu open
    s.worldLoaded = false;
    Scenario(s, Route::Impossible, Block::NoWorldToSave); // hero no longer loaded
    s = {}; s.jobActive = true;
    Scenario(s, Route::Wait, Block::Busy); // in-flight publication wins over fresh launch
    for (Health health : {Health::Missing, Health::Half, Health::Valid, Health::Stale, Health::Corrupt})
        for (Origin origin : {Origin::NewGame, Origin::World})
            for (unsigned mask = 0; mask < 256; ++mask)
                for (unsigned failures = 0; failures <= 3; ++failures) {
                    s = {};
                    s.health = health; s.origin = origin;
                    s.hooks = (mask & 1) != 0; s.worldEntered = (mask & 2) != 0;
                    s.worldLoaded = (mask & 4) != 0; s.worldRunning = (mask & 8) != 0;
                    s.quiet = (mask & 16) != 0; s.newAccount = (mask & 32) != 0;
                    s.jobActive = (mask & 64) != 0; s.recordKnown = (mask & 128) != 0;
                    if (s.worldRunning && !s.worldLoaded) continue;
                    s.failures = failures;
                    const Plan p = Decide(s);
                    const bool fresh = !s.worldEntered && !s.worldLoaded;
                    const bool usable = health == Health::Valid &&
                        (origin == Origin::NewGame || (!fresh && s.recordKnown && !s.newAccount));
                    if (!s.hooks) { assert(p.route == Route::Impossible && p.why == Block::Hooks); continue; }
                    if (usable) { assert(p.route == Route::Template && p.why == Block::None); continue; }
                    assert(p.route != Route::Template);
                    if (p.route == Route::Impossible) {
                        assert((s.worldEntered && !s.worldLoaded && !usable) ||
                               (s.recordKnown && s.newAccount && !fresh) || s.failures >= MaxCreateFailures);
                    }
                    if (health == Health::Valid) {
                        assert(origin == Origin::World);
                        if (fresh) assert(p.route == Route::FirstEntry && p.why == Block::None);
                        else if (!s.recordKnown) assert(p.route == Route::Wait && p.why == Block::Record);
                        else {
                            assert(s.newAccount);
                            assert(p.route == Route::Impossible && p.why == Block::CarriedHero);
                        }
                        continue;
                    }
                    if (s.jobActive) assert(p.route == Route::Wait && p.why == Block::Busy);
                    else if (fresh) assert(p.route == Route::FirstEntry && p.why == Block::None);
                    else if (!s.worldLoaded) assert(p.route == Route::Impossible && p.why == Block::NoWorldToSave);
                    else if (!s.recordKnown) assert(p.route == Route::Wait && p.why == Block::Record);
                    else if (s.newAccount) assert(p.route == Route::Impossible && p.why == Block::CarriedHero);
                    else if (failures >= MaxCreateFailures) assert(p.route == Route::Impossible && p.why == Block::Failed);
                    else if (!s.worldRunning || !s.quiet) assert(p.route == Route::Wait && p.why == Block::NotQuiet);
                    else assert(p.route == Route::Create && p.why == Block::None);
                    if (!s.worldRunning) assert(p.route != Route::Create);
                }
}

void Scheduling()
{
    Background b;
    const uint32_t t = 0xfffffff0u;
    b.Observe(true, t);
    assert(b.Attempts() == 0 && !b.Due(t + 14999) && b.Due(t + 15000));
    b.Observe(true, t + 15000);
    assert(b.Due(t + 15000));
    b.Tried(t + 15000);
    assert(b.Attempts() == 1 && !b.Due(t + 15000 + 119999) && b.Due(t + 15000 + 120000));
    b.Tried(t + 15000 + 120000);
    assert(b.Attempts() == 2 && !b.Due(t + 15000 + 120000 + 599999));
    assert(b.Due(t + 15000 + 120000 + 600000));
    b.Tried(t + 15000 + 120000 + 600000);
    assert(b.Attempts() == Background::MaxAttempts && !b.Due(t + 15000 + 120000 + 1200000));
    b.Observe(false, t + 800000);
    assert(b.Attempts() == 0 && !b.Due(t + 800000));
    b.Observe(true, t + 800000);
    assert(!b.Due(t + 814999) && b.Due(t + 815000));
}

template<class Enum, class Name>
bool UniqueNames(std::initializer_list<Enum> values, Name name)
{
    std::set<std::string> seen;
    bool unique = true;
    for (Enum value : values) {
        const char* text = name(value);
        assert(text && *text);
        if (!seen.insert(text).second) {
            std::cerr << "duplicate name: " << text << '\n';
            unique = false;
        }
    }
    return unique;
}
}

int main()
{
    Marker();
    Structure();
    Inspection();
    Publication();
    Scheduling();
    Decisions();
    assert(UniqueNames<Health>({Health::Missing, Health::Half, Health::Valid, Health::Stale, Health::Corrupt}, HealthName));
    assert(UniqueNames<Why>({Why::None, Why::NoFiles, Why::NoHeader, Why::NoWorld, Why::NoMarker, Why::HeaderEmpty,
                            Why::WorldEmpty, Why::HeaderSize, Why::WorldSize, Why::MarkerFormat, Why::MarkerVersion,
                            Why::GameUnknown, Why::GameChanged, Why::ProtocolChanged, Why::SizeMismatch, Why::CrcMismatch,
                            Why::HeaderFormat, Why::WorldFormat}, WhyName));
    assert(UniqueNames<Fail>({Fail::None, Fail::NativeRefused, Fail::NativeCalls, Fail::NoHeader, Fail::NoWorld,
                             Fail::HeaderEmpty, Fail::WorldEmpty, Fail::Busy, Fail::Folder, Fail::Cleanup, Fail::Seal,
                             Fail::Abandoned, Fail::AccessDenied, Fail::DiskFull}, FailName));
    const auto blocks = {Block::None, Block::NotQuiet, Block::Busy, Block::NoWorldToSave,
                         Block::CarriedHero, Block::Failed, Block::Hooks, Block::Record};
    assert(UniqueNames<Block>(blocks, BlockName));
    assert(std::string(BlockName(Block::Record)) == "record-pending");
    assert(std::string(BlockCode(Block::Record)) == "template-unavailable");
    for (Block block : blocks) {
        const char* code = BlockCode(block);
        assert(code && *code);
    }
    assert(UniqueNames<Block>({Block::NoWorldToSave, Block::CarriedHero, Block::Hooks}, BlockCode));
    RecoveryCornerCases();
    return 0;
}
