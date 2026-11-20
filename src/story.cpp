// SPDX-License-Identifier: GPL-3.0-or-later
#include "story_internal.h"

#include "arena.h"
#include "guide.h"
#include "log.h"
#include "reputation_policy.h"
#include "telemetry.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include <vector>

namespace g3t::story {
namespace detail {
bool disabled = false;
bool fullZeros = true;    // QA negative control (host, stage W4): storyzerosoff (the full state without zeros, as before 23.10.)
bool reloadReset = true;  // QA negative control (client, stageStoryReload): storyreloadoff
unsigned reloads = 0;
} // namespace detail

namespace {
using namespace detail;

constexpr DWORD PollInterval = 500;
DWORD lastPoll = 0;
bool admissionRequested = false;
bool admissionApplied = false;

struct InitialReplay {
    uint32_t generation = 0;
    bool captured = false;
    bool finished = false;
    uint8_t sent = 0;
    std::array<States, DomainCount> values;
    std::array<States, DomainCount> latest;
};
std::array<InitialReplay, net::MaxPlayers> initialReplays;

bool Send(Domain& d, std::vector<net::StateEntry> const& entries, uint8_t to = 0, bool snapshot = false, bool catchup = false)
{
    const uint8_t kind = d.kind | (snapshot ? policy::Snapshot : 0) | (catchup ? policy::Catchup : 0);
    auto ordered = entries;
    if (d.kind == net::StateQuestRuntime)
        std::stable_partition(ordered.begin(), ordered.end(), [](auto const& entry) {
            return policy::IsPackedCounterCount(entry.name);
        });
    for (size_t at = 0; at < ordered.size(); at += net::MaxStatesPerMessage) {
        const size_t left = ordered.size() - at;
        const int count = static_cast<int>(left < net::MaxStatesPerMessage ? left : net::MaxStatesPerMessage);
        if (to ? !net::SendStatesTo(to, kind, ordered.data() + at, count) :
                 !net::SendStates(kind, ordered.data() + at, count)) return false;
        d.sent += count;
    }
    return true;
}

bool Entry(Domain& d, std::string const& name, uint32_t value, std::vector<net::StateEntry>& out)
{
    net::StateEntry entry{static_cast<uint8_t>(value), {}};
    std::string packed = name;
    if (d.kind == net::StateQuestRuntime) {
        policy::ParsedRuntimeKey parsed;
        if (!policy::ParseRuntimeKey(name, parsed) ||
            !policy::PackRuntime(policy::NameId(parsed.target) + '\x1f' + policy::FormatRuntimeField(parsed),
                                 value, sizeof(entry.name), packed)) {
            Logf("story: quest runtime cannot be encoded name=%s; snapshot refused", name.c_str());
            ++d.skipped;
            return false;
        }
        entry.value = 1;
    } else if (d.kind == net::StateReputation) {
        if (!reputation::PackState(name, value, sizeof(entry.name), packed)) {
            Logf("story: reputation state cannot be encoded name=%s; snapshot refused", name.c_str());
            ++d.skipped;
            return false;
        }
        entry.value = 1;
    } else if (d.kind == net::StateQuests || d.kind == net::StateInfos) {
        packed = policy::NameId(name);
    }
    if (packed.size() >= sizeof(entry.name)) {
        if (d.kind == net::StateEvents) {
            Logf("story: event name too long (%zu bytes) name=%s; skipped", packed.size(), name.c_str());
            ++d.skipped;
            return true;
        }
        Logf("story: named state too long kind=%u name=%s; snapshot refused", d.kind, name.c_str());
        ++d.skipped;
        return false;
    }
    strcpy_s(entry.name, packed.c_str());
    out.push_back(entry);
    return true;
}

void Apply(Domain& d, char const* name, uint32_t value, bool restoring = false)
{
    if (restoring && d.kind == net::StateQuests) {
        if (auto* quest = Quests()->GetQuest(bCString(name)))
            quest->AccessStatus() = static_cast<gEQuestStatus>(value);
        ++d.applied;
        return;
    }
    if (!d.apply(name, value)) ++d.forced;
    ++d.applied;
}

void HandleSnapshotRequest(uint8_t from, Domain* d)
{
    if (!net::InWorld(1) || !net::PeerJoined(from)) return;
    // A client entered the game (or reconnected / reloaded to template): everything off its default, so it continues
    // the host's story. Existing peers already know it.
    auto& replay = initialReplays[from];
    const uint32_t generation = net::PeerGeneration(from);
    if (policy::ShouldResetInitialReplay(replay.generation, generation, replay.finished)) {
        replay = {};
        replay.generation = generation;
    }
    if (!replay.captured) {
        for (Domain const& domain : domains) if (!domain.ready()) return;
        for (Domain& domain : domains) domain.snapshot(replay.values[domain.kind - 1]);
        replay.latest = replay.values;
        replay.captured = true;
    }
    States const& current = replay.values[d->kind - 1];
    std::vector<net::StateEntry> all;
    size_t zeros = 0;
    for (auto const& [name, value] : current) {
        if (value != 0 && !Entry(*d, name, value, all)) return;
        else if (!value && fullZeros && d->zeroed.count(name) && Entry(*d, name, 0, all)) ++zeros;
    }
    const uint8_t wire = d->kind | policy::Snapshot;
    const bool sent = net::SendStatesTo(from, wire, nullptr, 0) && Send(*d, all, from, true) &&
                      net::SendStatesTo(from, wire | policy::End, nullptr, 0);
    if (sent) replay.sent |= static_cast<uint8_t>(1u << (d->kind - 1));
    if (!sent) Logf("story: full %s state queue failed player=%u; admission retry required", d->label, from);
    Logf("story: full %s state (%zu entries, %zu of them cleared here) for player %u", d->label, all.size(), zeros, from);
}

bool DecodeIncomingEntries(uint8_t from, Domain* d, bool snapshot, net::StateEntry const* entries, int count,
                           std::vector<std::pair<std::string, uint32_t>>& decoded)
{
    auto reject = [&]() {
        if (!net::IsHost() && snapshot) { d->baseline = {}; d->nextRequest = 0; }
        return false;
    };
    decoded.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        std::string name = entries[i].name;
        uint32_t value = entries[i].value;
        if (d->kind == net::StateQuestRuntime &&
            (entries[i].value != 1 || !policy::UnpackRuntime(name, name, value))) {
            Logf("story: malformed quest runtime from player=%u; ignored", from);
            return reject();
        }
        if (d->kind == net::StateReputation &&
            (entries[i].value != 1 || !reputation::UnpackState(name, name, value))) {
            Logf("story: malformed reputation state from player=%u; ignored", from);
            return reject();
        }
        if (d->kind == net::StateWorldClock) {
            net::ClockState state{};
            if (!snapshot || value > 1 || !policy::ReadClock(name, state)) {
                Log("story: invalid bootstrap clock; admission retry required");
                return reject();
            }
        }
        if (d->kind != net::StateEvents && d->kind != net::StateWorldClock && d->kind != net::StateReputation &&
            !ResolveNamedState(d->kind, name, value))
            return reject();
        if (d->kind == net::StateQuests && value > gEQuestStatus_Won) {
            Logf("story: invalid quest status from player=%u; ignored", from);
            return reject();
        }
        if (d->kind == net::StateInfos && value > 1) {
            Logf("story: invalid dialog-state flag from player=%u; ignored", from);
            return reject();
        }
        decoded.emplace_back(std::move(name), value);
    }
    return true;
}

void ApplyHostClientDelta(uint8_t from, Domain* d, bool snapshot, int count,
                          std::vector<std::pair<std::string, uint32_t>> const& decoded)
{
    if (d->kind == net::StateQuests) FlushReputation();
    for (auto const& entry : decoded) {
        gCQuest_PS* quest = nullptr;
        int prevQuestStatus = -1;
        if (d->kind == net::StateQuests && !snapshot) {
            if (!arena::AllowClientQuestStatusOnHost(from, entry.first.c_str(), static_cast<int>(entry.second)))
                continue;
            quest = Quests()->GetQuest(bCString(entry.first.c_str()));
            if (quest) prevQuestStatus = StatusOf(quest);
        }
        Apply(*d, entry.first.c_str(), entry.second);
        if (quest && prevQuestStatus >= 0) {
            const int nowStatus = StatusOf(quest);
            guide::OnClientFollowQuestRunning(from, entry.first.c_str(), prevQuestStatus, nowStatus);
            arena::OnClientArenaQuestStatus(from, entry.first.c_str(), prevQuestStatus, nowStatus);
        }
    }
    States current;
    d->snapshot(current);
    std::vector<net::StateEntry> result;
    for (auto const& entry : decoded) {
        auto it = current.find(entry.first);
        if (it == current.end()) continue;
        const auto prevIt = d->known.find(it->first);
        const uint32_t beforeVal = prevIt != d->known.end() ? prevIt->second : 0u;
        Track(*d, it->first, it->second);
        d->known[it->first] = it->second;
        Entry(*d, it->first, it->second, result);
        telemetry::OnStoryDelta("client_delta_on_host", from, d->label, it->first.c_str(), beforeVal, it->second);
    }
    Send(*d, result);
    if (d->kind == net::StateQuests) {
        arena::SyncArenaStatuses();
        FlushReputation();
    }
    Logf("story: applied %d %s change(s) from player %u", count, d->label, from);
}

void ApplyClientStates(Domain* d, bool catchup, std::vector<std::pair<std::string, uint32_t>> const& decoded)
{
    for (auto const& entry : decoded) {
        if (d->kind == net::StateWorldClock && !ClockApply(entry.first.c_str(), entry.second)) {
            admissionApplied = false; d->baseline = {}; d->nextRequest = 0; return;
        }
        const auto prevIt = d->known.find(entry.first);
        const uint32_t beforeVal = prevIt != d->known.end() ? prevIt->second : 0u;
        d->known[entry.first] = entry.second;
        if (d->kind != net::StateWorldClock) Apply(*d, entry.first.c_str(), entry.second, catchup);
        if (!catchup || beforeVal != entry.second)
            telemetry::OnStoryDelta(catchup ? "host_catchup" : "host_delta", 1, d->label, entry.first.c_str(), beforeVal, entry.second);
        if (d->kind == net::StateQuests) {
            guide::NoteQuestStatusFromHost(entry.first.c_str(), static_cast<int>(entry.second));
            arena::NoteQuestStatusFromHost(entry.first.c_str(), static_cast<int>(entry.second));
        }
    }
}

bool CommitBaselines()
{
    for (Domain const& d : domains) if (!d.baseline.complete || !d.ready()) return false;
    Domain& runtime = *Find(net::StateQuestRuntime);
    for (auto const& entry : runtime.baseline.values) {
        policy::ParsedRuntimeKey parsed;
        if (policy::ParseRuntimeKey(entry.first, parsed) && policy::IsCounterOutOfBounds(runtime.baseline.values, parsed)) {
            Logf("story: contradictory quest counter baseline name=%s; admission retry required", parsed.target.c_str());
            runtime.baseline = {};
            runtime.nextRequest = 0;
            return false;
        }
    }
    // Clock and runtime precede statuses; no Running quest evaluates against the fresh world's date.
    for (Domain& d : domains) {
        States current;
        d.snapshot(current);
        for (auto const& entry : current) {
            policy::ParsedRuntimeKey parsed;
            if (d.kind == net::StateQuestRuntime && policy::ParseRuntimeKey(entry.first, parsed) &&
                policy::IsCounterOutOfBounds(d.baseline.values, parsed))
                continue;
            if (!d.baseline.values.count(entry.first)) d.baseline.values[entry.first] = 0;
        }
    }
    Domain& clockDomain = *Find(net::StateWorldClock);
    for (auto const& entry : clockDomain.baseline.values) {
        if (entry.second && !ClockApply(entry.first.c_str(), entry.second)) {
            Log("story: bootstrap clock apply failed; admission retry required");
            clockDomain.baseline = {};
            clockDomain.nextRequest = 0;
            return false;
        }
    }
    for (int phase = 0; phase < 2; ++phase)
        for (auto const& entry : runtime.baseline.values)
            if ((entry.first.back() == 'n') == (phase == 0)) Apply(runtime, entry.first.c_str(), entry.second, true);
    for (Domain& d : domains) {
        if (d.kind != net::StateQuestRuntime && d.kind != net::StateWorldClock) {
            for (auto const& entry : d.baseline.values) {
                Apply(d, entry.first.c_str(), entry.second, true);
                if (d.kind == net::StateQuests) {
                    guide::NoteQuestStatusFromHost(entry.first.c_str(), static_cast<int>(entry.second));
                    arena::NoteQuestStatusFromHost(entry.first.c_str(), static_cast<int>(entry.second));
                }
            }
        }
        d.known.clear();
        for (auto const& entry : d.baseline.values) d.known[entry.first] = entry.second;
        d.primed = true;
        d.wasConnected = true;
        Logf("story: authoritative %s baseline restored entries=%zu rewards=0", d.label, d.known.size());
        telemetry::OnStoryDelta("baseline_committed", 1, d.label, "snapshot_entries", 0, static_cast<uint32_t>(d.known.size()));
    }
    arena::SyncArenaStatuses();
    admissionApplied = true;
    return true;
}

void OnStates(uint8_t from, uint8_t kind, net::StateEntry const* entries, int count)
{
    const bool snapshot = (kind & policy::Snapshot) != 0, end = (kind & policy::End) != 0;
    const bool catchup = (kind & policy::Catchup) != 0;
    Domain* d = Find(kind & policy::KindMask);
    if (disabled || !d || !d->ready()) return;
    if (catchup && (net::IsHost() || from != 1 || !snapshot || end || count <= 0)) return;
    d->received += count;
    if (net::IsHost() && count == 0 && snapshot && !end) {
        HandleSnapshotRequest(from, d);
        return;
    }
    if ((net::IsHost() && snapshot) || (!net::IsHost() && (!admissionRequested || (snapshot && !catchup && d->baseline.complete)))) return;
    if (net::IsHost() && d->kind == net::StateReputation) {
        Logf("story: client reputation state from player=%u refused; host owns reputation", from);
        return;
    }
    if (!net::IsHost() && snapshot && !count) {
        if (!end) { d->baseline.Begin(); return; }
        if (!d->baseline.Finish()) return;
        CommitBaselines();
        return;
    }
    std::vector<std::pair<std::string, uint32_t>> decoded;
    if (!DecodeIncomingEntries(from, d, snapshot, entries, count, decoded)) return;
    if (!net::IsHost() && snapshot && !catchup) {
        for (auto const& entry : decoded)
            if (!d->baseline.Add(entry.first, entry.second)) return;
        return;
    }
    if (!net::IsHost() && !admissionApplied) {
        if (d->baseline.begun)
            for (auto const& entry : decoded)
                policy::MergeLiveBaselineDelta(
                    d->baseline.values, d->kind == net::StateQuestRuntime, entry.first, entry.second);
        return;
    }
    if (net::IsHost()) ApplyHostClientDelta(from, d, snapshot, count, decoded);
    else ApplyClientStates(d, catchup, decoded);
}

void Poll(Domain& d)
{
    if (!net::IsHost() && !net::Joined()) d.wasConnected = false;
    if (!d.ready() || (net::IsHost() ? !net::InWorld(1) : !net::Joined() || !admissionRequested)) return;
    if (!net::IsHost() && !admissionApplied) {
        const DWORD now = GetTickCount();
        if (!d.baseline.complete && static_cast<int32_t>(now - d.nextRequest) >= 0) {
            d.nextRequest = now + 1000;
            net::SendStates(d.kind | policy::Snapshot, nullptr, 0);
            ++d.requests;
        }
        return;
    }
    if (d.kind == net::StateWorldClock) return; // Ordinary time sharing remains clock.cpp's existing protocol.
    if (d.kind == net::StateReputation && !net::IsHost()) return; // Host is sole authority on camp/enclave reputation.
    if (!net::IsHost() && !net::InWorld(net::LocalId())) return;
    States current;
    d.snapshot(current);
    if (!d.primed) {
        // Values before this point are the new game's own defaults, identical on every machine.
        for (auto const& [name, value] : current)
            if (!d.known.count(name)) d.known[name] = value;
        d.primed = true;
    }
    d.wasConnected = net::IsHost() || net::Joined();
    std::vector<net::StateEntry> changes;
    for (auto const& [name, value] : current) {
        auto it = d.known.find(name);
        if (it != d.known.end() && it->second == value) continue;
        const uint32_t beforeVal = it != d.known.end() ? it->second : 0u;
        Logf("story: local %s %s=%d", d.label, name.c_str(), static_cast<int>(value));
        telemetry::OnStoryDelta("local_change", net::LocalId(), d.label, name.c_str(), beforeVal, value);
        Track(d, name, value);
        d.known[name] = value;
        Entry(d, name, value, changes);
    }
    if (changes.empty()) return;
    if (!net::IsHost()) d.claims += static_cast<unsigned>(changes.size());
    Send(d, changes);
}
} // namespace

void Start() { net::SetStateHandler(&OnStates); }

void PrepareAdmission()
{
    if (net::IsHost()) return;
    admissionRequested = true;
    admissionApplied = false;
    for (Domain& d : domains) {
        d.baseline = {}; d.wasConnected = false; d.nextRequest = 0;
    }
}

bool AdmissionReady()
{
    if (net::IsHost()) return true;
    return admissionRequested && admissionApplied;
}

bool AdmissionStarted() { return admissionRequested; }

void FlushReputation()
{
    if (disabled || !net::IsHost()) return;
    if (Domain* d = Find(net::StateReputation)) Poll(*d);
}

void FlushQuests()
{
    if (disabled) return;
    arena::ObserveLocalQuests();
    if (Domain* rt = Find(net::StateQuestRuntime)) Poll(*rt);
    if (Domain* q = Find(net::StateQuests)) Poll(*q);
    if (Domain* inf = Find(net::StateInfos)) Poll(*inf);
}

bool FinishAdmission(unsigned char player)
{
    if (!net::IsHost() || player < 2 || player >= net::MaxPlayers) return false;
    auto& replay = initialReplays[player];
    if (!replay.captured || replay.sent != AllDomainsSentMask || replay.generation != net::PeerGeneration(player)) return false;
    std::array<States, DomainCount> captured;
    for (Domain& domain : domains) {
        if (!domain.ready()) return false;
        domain.snapshot(captured[domain.kind - 1]);
    }
    std::unordered_set<std::string> changedQuests;
    for (auto const& entry : policy::Changes(replay.latest[net::StateQuests - 1], captured[net::StateQuests - 1]))
        changedQuests.insert(entry.first);
    for (uint8_t kind : {net::StateWorldClock, net::StateQuestRuntime, net::StateQuests, net::StateReputation, net::StateInfos, net::StateEvents}) {
        Domain& d = *Find(kind);
        States& current = captured[d.kind - 1];
        States delta = policy::Changes(replay.latest[d.kind - 1], current);
        if (kind == net::StateQuestRuntime) {
            for (auto const& entry : delta) {
                policy::ParsedRuntimeKey parsed;
                if (policy::ParseRuntimeKey(entry.first, parsed) && parsed.field != policy::RuntimeField::InfoEnd)
                    changedQuests.insert(parsed.target);
            }
            // Resizing a delivery counter requires its full row, not just the one changed element.
            for (auto const& entry : current) {
                policy::ParsedRuntimeKey parsed;
                if (policy::ParseRuntimeKey(entry.first, parsed) && parsed.field != policy::RuntimeField::InfoEnd &&
                    changedQuests.count(parsed.target))
                    delta[entry.first] = entry.second;
            }
        }
        std::vector<net::StateEntry> changes;
        for (auto const& entry : delta) {
            policy::ParsedRuntimeKey parsed;
            if (d.kind == net::StateQuestRuntime && policy::ParseRuntimeKey(entry.first, parsed) &&
                policy::IsCounterOutOfBounds(current, parsed))
                continue;
            if (!Entry(d, entry.first, entry.second, changes)) return false;
        }
        if (!Send(d, changes, player, true, true)) return false;
        replay.latest[d.kind - 1] = std::move(current);
    }
    replay.finished = true;
    return true;
}

// The client's known values were the host's as far as it knew them in the world before. After a load (the host's save,
// or its own) the first poll took every difference to the loaded save for its own player's changes and sent them; the
// host applied them, and what it changed during the client's load went back to the save's value (review 26.10.: a quest
// back to running, a told dialog option offered again). Primed anew, the client takes the loaded state as it is, as at
// its first join, and asks for the host's full state.
void OnWorldChange()
{
    ResetDomainCaches();
    initialReplays = {};
    if (!reloadReset && !net::IsHost()) return;
    if (!net::IsHost()) {
        admissionRequested = false;
        admissionApplied = false;
    }
    for (Domain& d : domains) {
        d.known.clear();
        d.zeroed.clear();
        d.primed = false;
        d.wasConnected = false;
        d.baseline = {};
    }
    ++reloads;
}

void Tick()
{
    if (disabled) return;
    const DWORD now = GetTickCount();
    if (now - lastPoll < PollInterval) return;
    lastPoll = now;
    arena::ObserveLocalQuests();
    for (Domain& d : domains) Poll(d);
}
} // namespace g3t::story
