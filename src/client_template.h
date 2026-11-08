// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-free policy for the client's rejoin template (the native save "G3T_client_template" that the engine's own LoadGame
// enters): what a complete template is (marker v2), what is on disk (Health), how a save completes (Publish: the engine writes
// the world half on another thread, ~0.3 s after HUD.SaveGame returned) and which way back into the host's world to take
// (Decide). persist.cpp builds the facts and performs the file/native work; client_template_store.h does the file I/O.
#pragma once

#include "client_rejoin.h"
#include "saving_policy.h"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace g3t::rejoin::tmpl {
constexpr uint32_t MarkerVersion = 2;
constexpr uint64_t MinHeaderBytes = 1024, MaxHeaderBytes = 64ull * 1024 * 1024;   // a real header is ~65 KB (it holds the thumbnail)
constexpr uint64_t MinWorldBytes = 64ull * 1024, MaxWorldBytes = 128ull * 1024 * 1024; // a real starting world is ~2 MB
constexpr uint32_t SettleMs = 8000;       // PUP's world writer finishes within ~0.3 s here; allow slow CPUs, disks and scanners
constexpr uint32_t RecoverAfterMs = 1500; // an ignored native rename is repaired only after the writer had its chance
constexpr uint32_t RecoverStableMs = 500; // ... and only when the finished staging file stayed unlocked this long
constexpr uint32_t PollMs = 50;
constexpr unsigned MaxCreateFailures = 2; // in-world creations in a row before the exact reason is shown instead of retrying

// Where the hero inside the template comes from. A New Game hero is fresh; a hero saved from a played world is carried
// and must never start a NEW account (a new account only ever gets a freshly initialized hero).
enum class Origin : uint8_t { NewGame, World };
inline char const* OriginName(Origin origin) { return origin == Origin::World ? "world" : "new-game"; }

// What the template must match to be usable: the installed Game.dll and this mod's protocol.
struct Identity {
    uint64_t game = 0;
    uint16_t protocol = saving::Protocol;
};

// The marker (.g3t) is written last; a template without a matching marker does not exist.
struct Seal {
    uint32_t version = MarkerVersion;
    uint64_t game = 0;
    uint16_t protocol = 0;
    Origin origin = Origin::NewGame;
    uint64_t headerSize = 0, worldSize = 0;
    uint32_t headerCrc = 0, worldCrc = 0;
};
inline std::string Encode(Seal const& seal)
{
    char text[256];
    std::snprintf(text, sizeof(text),
                  "G3T client template %u\ngame=%llu\nprotocol=%u\norigin=%s\nheader=%llu crc=%08x\nworld=%llu crc=%08x\n",
                  seal.version, static_cast<unsigned long long>(seal.game), static_cast<unsigned>(seal.protocol),
                  OriginName(seal.origin), static_cast<unsigned long long>(seal.headerSize), seal.headerCrc,
                  static_cast<unsigned long long>(seal.worldSize), seal.worldCrc);
    return text;
}
enum class Parse : uint8_t { Ok, Empty, Format, Version };
namespace detail {
inline bool Number(std::string const& text, uint64_t& value, int base = 10)
{
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return result.ec == std::errc() && result.ptr == text.data() + text.size();
}
inline bool Field(std::string const& line, char const* key, std::string& rest)
{
    const size_t n = std::strlen(key);
    if (line.size() <= n || line.compare(0, n, key) != 0 || line[n] != '=') return false;
    rest = line.substr(n + 1);
    return true;
}
// "<size> crc=<hex8>"
inline bool SizeCrc(std::string const& rest, uint64_t& size, uint32_t& crc)
{
    const size_t space = rest.find(' ');
    if (space == std::string::npos || rest.compare(space, 5, " crc=") != 0) return false;
    uint64_t crcValue = 0;
    const std::string hex = rest.substr(space + 5);
    if (hex.size() != 8 || !Number(rest.substr(0, space), size) || !Number(hex, crcValue, 16)) return false;
    crc = static_cast<uint32_t>(crcValue);
    return true;
}
}
template<class Bytes>
inline Parse Decode(Bytes const& bytes, Seal& out)
{
    if (bytes.empty()) return Parse::Empty;
    const std::string text(reinterpret_cast<char const*>(bytes.data()), bytes.size());
    std::vector<std::string> lines;
    for (size_t at = 0; at < text.size();) {
        const size_t end = text.find('\n', at);
        if (end == std::string::npos) return Parse::Format; // the marker always ends with a newline
        lines.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    static constexpr char head[] = "G3T client template ";
    if (lines.empty() || lines[0].compare(0, sizeof(head) - 1, head) != 0) return Parse::Format;
    uint64_t version = 0;
    if (!detail::Number(lines[0].substr(sizeof(head) - 1), version)) return Parse::Format;
    if (version != MarkerVersion) return Parse::Version;
    if (lines.size() != 6) return Parse::Format;
    Seal seal;
    std::string rest;
    uint64_t number = 0;
    if (!detail::Field(lines[1], "game", rest) || !detail::Number(rest, seal.game)) return Parse::Format;
    if (!detail::Field(lines[2], "protocol", rest) || !detail::Number(rest, number) || number > 0xffff) return Parse::Format;
    seal.protocol = static_cast<uint16_t>(number);
    if (!detail::Field(lines[3], "origin", rest)) return Parse::Format;
    if (rest == "new-game") seal.origin = Origin::NewGame;
    else if (rest == "world") seal.origin = Origin::World;
    else return Parse::Format;
    if (!detail::Field(lines[4], "header", rest) || !detail::SizeCrc(rest, seal.headerSize, seal.headerCrc)) return Parse::Format;
    if (!detail::Field(lines[5], "world", rest) || !detail::SizeCrc(rest, seal.worldSize, seal.worldCrc)) return Parse::Format;
    out = seal;
    return Parse::Ok;
}

// ---- what is on disk ----

struct File {
    bool exists = false;
    bool locked = false;        // exists but a writer holds it (the engine's world writer, a scanner)
    bool folderMissing = false; // the save folder itself is unavailable
    uint64_t size = 0;
    uint64_t stamp = 0;         // last write time (FILETIME ticks): changes when the file was replaced or rewritten
    uint32_t error = 0;         // system error of the probe (0 when the file was seen)
};
struct Facts {
    File header, world, marker;          // the three published files
    File headerTmp, worldTmp, markerTmp; // the engine's / our staging files
};
// Identity of the three published files, so the deep check runs only after one of them changed.
inline uint64_t Signature(Facts const& facts)
{
    uint64_t hash = 1469598103934665603ull;
    for (File const* file : {&facts.header, &facts.world, &facts.marker}) {
        for (uint64_t part : {static_cast<uint64_t>(file->exists), file->size, file->stamp}) {
            hash ^= part;
            hash *= 1099511628211ull;
        }
    }
    return hash;
}
enum class Health : uint8_t { Missing, Half, Valid, Stale, Corrupt };
enum class Why : uint8_t {
    None, NoFiles, NoHeader, NoWorld, NoMarker, HeaderEmpty, WorldEmpty, HeaderSize, WorldSize, MarkerFormat,
    MarkerVersion, GameUnknown, GameChanged, ProtocolChanged, SizeMismatch, CrcMismatch, HeaderFormat, WorldFormat
};
inline char const* HealthName(Health health)
{
    switch (health) {
    case Health::Missing: return "missing";
    case Health::Half: return "half";
    case Health::Valid: return "valid";
    case Health::Stale: return "stale";
    case Health::Corrupt: return "corrupt";
    }
    return "?";
}
inline char const* WhyName(Why why)
{
    switch (why) {
    case Why::None: return "-";
    case Why::NoFiles: return "no-files";
    case Why::NoHeader: return "no-header";
    case Why::NoWorld: return "no-world";
    case Why::NoMarker: return "no-marker";
    case Why::HeaderEmpty: return "header-empty";
    case Why::WorldEmpty: return "world-empty";
    case Why::HeaderSize: return "header-size";
    case Why::WorldSize: return "world-size";
    case Why::MarkerFormat: return "marker-format";
    case Why::MarkerVersion: return "marker-version";
    case Why::GameUnknown: return "game-unknown";
    case Why::GameChanged: return "game-changed";
    case Why::ProtocolChanged: return "protocol-changed";
    case Why::SizeMismatch: return "size-mismatch";
    case Why::CrcMismatch: return "crc-mismatch";
    case Why::HeaderFormat: return "header-format";
    case Why::WorldFormat: return "world-format";
    }
    return "?";
}
// Structure of an engine-written CP175 pair (static analysis of HUD.SaveGame / SaveGameWorld, confirmed on live files):
// the header starts "GENOMESG" + the CP token (0xAF, 0xB0 with Alternative Balancing); the world starts "GENOMFLE", the
// uncompressed size, the compressed size (= file size - 16) and the zlib payload. Not a full loadability proof.
template<class Bytes>
inline Why HeaderStructure(Bytes const& bytes)
{
    if (bytes.size() < 10 || std::memcmp(bytes.data(), "GENOMESG", 8) != 0) return Why::HeaderFormat;
    const unsigned token = static_cast<unsigned>(bytes[8]) | (static_cast<unsigned>(bytes[9]) << 8);
    return token == 0xAF || token == 0xB0 ? Why::None : Why::HeaderFormat;
}
template<class Bytes>
inline Why WorldStructure(Bytes const& bytes)
{
    if (bytes.size() < 17 || std::memcmp(bytes.data(), "GENOMFLE", 8) != 0) return Why::WorldFormat;
    const auto word = [&](size_t at) {
        return static_cast<uint32_t>(bytes[at]) | (static_cast<uint32_t>(bytes[at + 1]) << 8) |
               (static_cast<uint32_t>(bytes[at + 2]) << 16) | (static_cast<uint32_t>(bytes[at + 3]) << 24);
    };
    return word(8) != 0 && word(12) == bytes.size() - 16 ? Why::None : Why::WorldFormat;
}
struct Report {
    Health health = Health::Missing;
    Why why = Why::NoFiles;
    bool sealed = false; // seal below was read from a well-formed marker
    Seal seal;
};

// Cheap judgement from sizes and the parsed marker. Anything but Valid is removed before the next save; stale and corrupt
// templates are regenerated rather than loaded.
inline Report Inspect(Facts const& facts, Parse parsed, Seal const& marker, Identity const& identity)
{
    Report report;
    const bool any = facts.header.exists || facts.world.exists || facts.marker.exists;
    if (!any) return {Health::Missing, Why::NoFiles, false, {}};
    if (!facts.header.exists) return {Health::Half, Why::NoHeader, false, {}};
    if (!facts.world.exists) return {Health::Half, Why::NoWorld, false, {}};
    if (!facts.marker.exists) return {Health::Half, Why::NoMarker, false, {}};
    if (parsed == Parse::Version) return {Health::Stale, Why::MarkerVersion, false, {}};
    if (parsed != Parse::Ok) return {Health::Corrupt, Why::MarkerFormat, false, {}};
    report.sealed = true;
    report.seal = marker;
    if (!identity.game) { report.health = Health::Stale; report.why = Why::GameUnknown; return report; }
    if (marker.game != identity.game) { report.health = Health::Stale; report.why = Why::GameChanged; return report; }
    if (marker.protocol != identity.protocol) { report.health = Health::Stale; report.why = Why::ProtocolChanged; return report; }
    report.health = Health::Corrupt;
    if (!facts.header.size) { report.why = Why::HeaderEmpty; return report; }
    if (!facts.world.size) { report.why = Why::WorldEmpty; return report; }
    if (facts.header.size < MinHeaderBytes || facts.header.size > MaxHeaderBytes) { report.why = Why::HeaderSize; return report; }
    if (facts.world.size < MinWorldBytes || facts.world.size > MaxWorldBytes) { report.why = Why::WorldSize; return report; }
    if (facts.header.size != marker.headerSize || facts.world.size != marker.worldSize) { report.why = Why::SizeMismatch; return report; }
    report.health = Health::Valid;
    report.why = Why::None;
    return report;
}
// The deep judgement: the bytes that are there are the bytes that were sealed.
inline Report Verify(Report report, uint32_t headerCrc, uint32_t worldCrc)
{
    if (report.health != Health::Valid) return report;
    if (headerCrc != report.seal.headerCrc || worldCrc != report.seal.worldCrc) {
        report.health = Health::Corrupt;
        report.why = Why::CrcMismatch;
    }
    return report;
}

// ---- one save: engine call -> settle -> seal ----

enum class Fail : uint8_t {
    None, NativeRefused, NativeCalls, NoHeader, NoWorld, HeaderEmpty, WorldEmpty, Busy, Folder, Cleanup, Seal, Abandoned,
    AccessDenied, DiskFull
};
inline char const* FailName(Fail fail)
{
    switch (fail) {
    case Fail::None: return "none";
    case Fail::NativeRefused: return "template-native-refused";
    case Fail::NativeCalls: return "template-native-calls";
    case Fail::NoHeader: return "template-no-header";
    case Fail::NoWorld: return "template-no-world";
    case Fail::HeaderEmpty: return "template-header-empty";
    case Fail::WorldEmpty: return "template-world-empty";
    case Fail::Busy: return "template-busy";
    case Fail::Folder: return "template-folder";
    case Fail::Cleanup: return "template-cleanup";
    case Fail::Seal: return "template-seal";
    case Fail::Abandoned: return "template-abandoned";
    case Fail::AccessDenied: return "save-access-denied";
    case Fail::DiskFull: return "disk-full";
    }
    return "?";
}
enum class Step : uint8_t { Wait, RecoverHeader, RecoverWorld, Seal, Failed };
enum class Phase : uint8_t { Idle, Settling, Done };

// The engine returns from HUD.SaveGame with the header complete and the world still being written into <slot>.g3savcpdat.tmp
// by another thread (exclusive; a read gets ERROR_SHARING_VIOLATION) which then renames it. A caller that checks the files
// right after the call finds no world and used to delete the valid header: wait for the facts instead, bounded.
class Publish {
    Phase phase_ = Phase::Idle;
    Origin origin_ = Origin::NewGame;
    uint32_t started_ = 0, native_ = 0, lastPoll_ = 0, candidateSince_ = 0;
    uint8_t candidate_ = 0; // 0 none, 1 header, 2 world: the staging file the stability timer is running for
    bool polled_ = false;
    Fail fail_ = Fail::None;

    static bool Reached(uint32_t now, uint32_t at) { return static_cast<int32_t>(now - at) >= 0; }
    static bool Settled(File const& f) { return f.exists && f.size > 0 && !f.locked; }

  public:
    Phase phase() const { return phase_; }
    bool Active() const { return phase_ == Phase::Settling; }
    Origin origin() const { return origin_; }
    Fail failure() const { return fail_; }
    uint32_t Elapsed(uint32_t now) const { return now - started_; }

    // The native call is about to be made (own files and their staging files are already removed).
    void Begin(Origin origin, uint32_t now)
    {
        phase_ = Phase::Settling;
        origin_ = origin;
        started_ = native_ = now;
        candidate_ = 0;
        candidateSince_ = 0;
        polled_ = false;
        fail_ = Fail::None;
    }
    // What HUD.SaveGame reported; the world half may still be in flight.
    void NativeReturned(bool ok, unsigned lowerCalls, bool lowerGood, uint32_t now)
    {
        native_ = now;
        if (!ok) Abort(Fail::NativeRefused);
        else if (lowerCalls != 1 || !lowerGood) Abort(Fail::NativeCalls);
    }
    void Abort(Fail why)
    {
        phase_ = Phase::Done;
        fail_ = why;
    }
    void Finish(bool ok, Fail why = Fail::Seal)
    {
        phase_ = Phase::Done;
        fail_ = ok ? Fail::None : why;
    }
    // Polling is throttled; ready() tells the caller whether this frame should look at the files at all.
    bool PollDue(uint32_t now) const { return Active() && (!polled_ || Reached(now, lastPoll_ + PollMs)); }
    Step Poll(Facts const& f, uint32_t now)
    {
        if (!Active()) return Step::Failed;
        polled_ = true;
        lastPoll_ = now;
        const uint32_t since = now - native_;
        if (Settled(f.header) && Settled(f.world)) return Step::Seal;
        // The settle window is a hard bound: a recovery request can never keep the save active past it.
        if (since >= SettleMs) {
            fail_ = Reason(f);
            phase_ = Phase::Done;
            return Step::Failed;
        }
        // The engine ignores a failed final rename (MoveFileA): a finished, unlocked staging file is moved by us, but only
        // after the writer had its chance, only when it stayed finished (unlocked) for RecoverStableMs and only onto a final
        // name that does not exist.
        const bool headerCandidate = !f.header.exists && Settled(f.headerTmp);
        const bool worldCandidate = !f.world.exists && Settled(f.worldTmp);
        if (since >= RecoverAfterMs && (headerCandidate || worldCandidate)) {
            const uint8_t which = headerCandidate ? 1 : 2;
            if (candidate_ != which) { candidate_ = which; candidateSince_ = now; }
            if (now - candidateSince_ >= RecoverStableMs) return headerCandidate ? Step::RecoverHeader : Step::RecoverWorld;
        } else candidate_ = 0;
        return Step::Wait;
    }
    // The exact reason when the settle window closed, header first (it is complete at the native return).
    static Fail Reason(Facts const& f)
    {
        if (f.header.folderMissing || f.world.folderMissing) return Fail::Folder;
        if (!f.header.exists) return f.headerTmp.exists ? (f.headerTmp.locked ? Fail::Busy : Fail::NoHeader) : Fail::NoHeader;
        if (f.header.locked) return Fail::Busy;
        if (!f.header.size) return Fail::HeaderEmpty;
        if (!f.world.exists) return f.worldTmp.exists ? (f.worldTmp.locked ? Fail::Busy : Fail::NoWorld) : Fail::NoWorld;
        if (f.world.locked) return Fail::Busy;
        if (!f.world.size) return Fail::WorldEmpty;
        return Fail::Seal;
    }
};

// ---- the way back into the host's world ----

enum class Route : uint8_t {
    Template,   // LoadGame of the valid template
    Create,     // make the template from the running world first (on demand), then enter
    FirstEntry, // title screen of a fresh launch: the native New Game (the only place it is safe), template follows
    Wait,       // the way exists but not at this moment (dialogue, trade, save exchange, a save in flight)
    Impossible  // no way without restarting Gothic 3 or fixing a hard failure: say exactly why
};
enum class Block : uint8_t { None, NotQuiet, Busy, NoWorldToSave, CarriedHero, Failed, Hooks, Record };
inline char const* BlockName(Block block)
{
    switch (block) {
    case Block::None: return "-";
    case Block::NotQuiet: return "not-quiet";
    case Block::Busy: return "template-save-running";
    case Block::NoWorldToSave: return "no-world-to-save";
    case Block::CarriedHero: return "carried-hero-new-account";
    case Block::Failed: return "create-failed";
    case Block::Hooks: return "native-hooks";
    case Block::Record: return "record-pending";
    }
    return "?";
}
// Localization code of an impossible route (the failure itself supplies its own code for Block::Failed).
inline char const* BlockCode(Block block)
{
    switch (block) {
    case Block::NoWorldToSave: return "template-no-world-to-save";
    case Block::CarriedHero: return "template-carried-hero";
    case Block::Hooks: return "native-hooks";
    default: return "template-unavailable";
    }
}
struct Situation {
    Health health = Health::Missing;
    Origin origin = Origin::NewGame; // of the valid template
    bool hooks = true;               // native HUD.LoadGame / HUD.SaveGame verified
    bool worldEntered = false;       // this process entered a world: the native New Game is refused
    bool worldLoaded = false;        // a hero exists in the engine session (its menu may be open)
    bool worldRunning = false;       // ... and the session runs, so the world can be saved right now
    bool quiet = false;              // no dialogue/trade/save exchange/native work/loading
    bool recordKnown = true;         // the host answered whether it has a record for this account (or the wait was given up)
    bool newAccount = false;         // ... and it has none: only a fresh New Game hero may enter
    bool jobActive = false;          // a template save is in flight
    unsigned failures = 0;           // creations from a running world that failed in a row
};
struct Plan {
    Route route = Route::Impossible;
    Block why = Block::None;
};
// Only Route::Impossible may tell the player to restart; every other outcome gets in (now or after a bounded wait).
// A template made from a played world (Origin::World) carries that hero: it may enter an account the host has a record
// for (the record replaces the hero) but never starts a new account, and at a fresh launch the New Game is always better
// (a fresh hero for everyone, and it rebuilds a fresh-hero template).
inline Plan Decide(Situation const& s)
{
    if (!s.hooks) return {Route::Impossible, Block::Hooks};
    const bool freshLaunch = !s.worldEntered && !s.worldLoaded;
    if (s.health == Health::Valid) {
        if (s.origin == Origin::World) {
            if (freshLaunch) return {Route::FirstEntry, Block::None};
            if (!s.recordKnown) return {Route::Wait, Block::Record};
            if (s.newAccount) return {Route::Impossible, Block::CarriedHero};
        }
        return {Route::Template, Block::None};
    }
    if (s.jobActive) return {Route::Wait, Block::Busy};
    if (freshLaunch) return {Route::FirstEntry, Block::None};
    if (!s.worldLoaded) return {Route::Impossible, Block::NoWorldToSave};
    if (!s.recordKnown) return {Route::Wait, Block::Record};
    if (s.newAccount) return {Route::Impossible, Block::CarriedHero};
    if (s.failures >= MaxCreateFailures) return {Route::Impossible, Block::Failed};
    if (!s.worldRunning || !s.quiet) return {Route::Wait, Block::NotQuiet};
    return {Route::Create, Block::None};
}

// Silent regeneration while admitted and idle: 15 s after the need appears, then 2 min, then 10 min; three tries a launch.
class Background {
    static constexpr uint32_t Delays[3] = {15000, 120000, 600000};
    bool armed_ = false;
    unsigned attempts_ = 0;
    uint32_t due_ = 0;
    static bool Reached(uint32_t now, uint32_t at) { return static_cast<int32_t>(now - at) >= 0; }

  public:
    static constexpr unsigned MaxAttempts = 3;
    unsigned Attempts() const { return attempts_; }
    void Observe(bool needed, uint32_t now)
    {
        if (!needed) { armed_ = false; attempts_ = 0; return; }
        if (!armed_) { armed_ = true; due_ = now + Delays[0]; }
    }
    bool Due(uint32_t now) const { return armed_ && attempts_ < MaxAttempts && Reached(now, due_); }
    void Tried(uint32_t now)
    {
        ++attempts_;
        due_ = now + Delays[attempts_ < 3 ? attempts_ : 2];
    }
};
}
