// SPDX-License-Identifier: GPL-3.0-or-later
#include "story.h"

#include "body.h"
#include "combat.h"
#include "clock.h"
#include "log.h"
#include "net.h"
#include "npcs.h"
#include "remote.h"
#include "story_policy.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace g3t::story {
namespace {
// QA: an NPC by name, or "puppet<id>" for that player's puppet (its template name, PC_Hero, finds the hero itself).
Entity Named(char const* name)
{
    return !std::strncmp(name, "puppet", 6) ? Entity(remote::PuppetOf(std::atoi(name + 6))) : Entity::GetEntity(bCString(name));
}
}

namespace {
constexpr DWORD PollInterval = 500;
using States = std::unordered_map<std::string, uint32_t>;

struct Domain {
    uint8_t kind;
    char const* label;
    bool (*ready)();
    void (*snapshot)(States&);
    bool (*apply)(char const* name, uint32_t value);  // true if the game's own function did it, false if set directly
    States known;  // host: values last sent; client: the host's values as far as this client knows them
    // Host: names that went back to 0 while the game ran here. The full state sends their zeros: a client that loads the
    // host's save got them as set, since the save is older (Codex review 22.10. R8). All zeros would be thousands of infos.
    std::unordered_set<std::string> zeroed;
    bool primed = false;
    bool wasConnected = false;
    policy::Baseline baseline;
    DWORD nextRequest = 0;
    unsigned sent = 0, received = 0, applied = 0, forced = 0, claims = 0, requests = 0, skipped = 0;
};

bool disabled = false;
bool fullZeros = true;  // QA negative control (host, stage W4): storyzerosoff (the full state without zeros, as before 23.10.)
bool reloadReset = true;  // QA negative control (client, stageStoryReload): storyreloadoff
unsigned reloads = 0;
DWORD lastPoll = 0;
bool admissionRequested = false;
bool admissionApplied = false;
std::unordered_map<std::string, std::string> questNames, infoNames;
bool namesReady = false;
struct InitialReplay {
    uint32_t generation = 0;
    bool captured = false;
    uint8_t sent = 0;
    std::array<States, 5> values;
    std::array<States, 5> latest;
};
std::array<InitialReplay, net::MaxPlayers> initialReplays;

void Track(Domain& d, std::string const& name, uint32_t value)
{
    if (!net::IsHost()) return;
    auto it = d.known.find(name);
    if (value) d.zeroed.erase(name);
    else if (it != d.known.end() && it->second) d.zeroed.insert(name);
}
bool Running() { return gCSession::GetInstance().IsGameRunning(); }

// --- quests ---
gCQuestManager_PS* Quests() { return Running() ? gCQuestManager_PS::GetInstance() : nullptr; }
uint8_t StatusOf(gCQuest_PS* quest) { return static_cast<uint8_t>(static_cast<gEQuestStatus const&>(quest->AccessStatus())); }
bool QuestsReady() { return Quests() != nullptr; }

void QuestSnapshot(States& states)
{
    auto& all = Quests()->AccessQuests();
    for (int i = 0; i < all.GetCount(); ++i)
        if (gCQuest_PS* quest = static_cast<gCQuest_PS*>(all.AccessAt(i))) states[quest->AccessName().GetText()] = StatusOf(quest);
}

// Through the game's own functions where one exists (journal entries, experience), else set directly.
bool QuestApply(char const* name, uint32_t status)
{
    gCQuestManager_PS* manager = Quests();
    const bCString id(name);
    gCQuest_PS* quest = manager->GetQuest(id);
    if (!quest || StatusOf(quest) == status) return true;
    const gEQuestStatus target = static_cast<gEQuestStatus>(status);
    if ((target == gEQuestStatus_Success || target == gEQuestStatus_Failed) && StatusOf(quest) == gEQuestStatus_Open)
        manager->RunQuest(id);
    if (target == gEQuestStatus_Running) manager->RunQuest(id);
    else if (target == gEQuestStatus_Success) manager->SucceedQuest(id);
    else if (target == gEQuestStatus_Failed) manager->FailQuest(id);
    if (StatusOf(quest) == status) return true;
    quest->SetStatus(target);
    return false;
}

void QuestRuntimeSnapshot(States& states)
{
    auto& all = Quests()->AccessQuests();
    for (int i = 0; i < all.GetCount(); ++i) {
        auto* quest = static_cast<gCQuest_PS*>(all.AccessAt(i));
        if (!quest) continue;
        const std::string key = std::string(quest->AccessName().GetText()) + '\x1f';
        states[key + "y"] = quest->AccessActivationTime_Year();
        states[key + "d"] = quest->AccessActivationTime_Day();
        states[key + "h"] = quest->AccessActivationTime_Hour();
        auto& counters = quest->AccessDeliveryCounter();
        states[key + "n"] = counters.GetCount();
        for (int c = 0; c < counters.GetCount(); ++c)
            states[key + "c" + std::to_string(c)] = static_cast<uint32_t>(counters.AccessAt(c));
    }
}
bool QuestRuntimeApply(char const* name, uint32_t value)
{
    std::string id, field;
    if (!policy::RuntimeKey(name, id, field)) return false;
    auto* quest = Quests()->GetQuest(bCString(id.c_str()));
    if (!quest) return false;
    if (field == "y") quest->AccessActivationTime_Year() = value;
    else if (field == "d") quest->AccessActivationTime_Day() = value;
    else if (field == "h") quest->AccessActivationTime_Hour() = value;
    else {
        auto& counters = quest->AccessDeliveryCounter();
        if (field == "n") {
            if (value > 512) return false;
            const int previous = counters.GetCount();
            counters.SetCount(static_cast<int>(value));
            for (int i = previous; i < counters.GetCount(); ++i) counters.AccessAt(i) = 0;
        } else {
            uint32_t index = 0;
            if (!policy::Number(field.substr(1), index) || index >= static_cast<uint32_t>(counters.GetCount())) return false;
            counters.AccessAt(static_cast<int>(index)) = static_cast<GEI32>(value);
        }
    }
    return true;
}

// --- dialog options ---
gCInfoManager_PS* Infos() { return Running() ? gCInfoManager_PS::GetInstance() : nullptr; }
bool InfosReady() { return Infos() != nullptr; }

void InfoSnapshot(States& states)
{
    auto& all = Infos()->AccessInfos();
    for (int i = 0; i < all.GetCount(); ++i)
        if (gCInfo_PS* info = all.AccessAt(i)) states[info->AccessName().GetText()] = info->AccessInfoGiven() ? 1 : 0;
}

bool InfoApply(char const* name, uint32_t given)
{
    if (gCInfo_PS* info = Infos()->GetInfo(bCString(name))) info->AccessInfoGiven() = given != 0;
    return false;  // no game function marks a dialog as told without running it
}

// --- story events the hero knows ---
// Dialogs set them on the hero who holds the dialog (SetGameEvent / ClearGameEvent, kept in the hero's
// PlayerMemory.PlayerKnows); other dialogs check them. Shared like quests, so that every hero knows what one
// learned (the client's Diego dialog set Diego_WarIsLost on the client's hero only; stage H exploration).
std::unordered_map<std::string, uint8_t> everSeen;  // events that were set at some point: 0 once cleared
// The chapter lives beside them in the hero's PlayerMemory (SetChapter, info command 53: 2 once a god is chosen) and decides
// attitudes through Entity::GetPlayer (re/out/notify-enclave-predicate-20261024-c.c): shared as one more entry of this kind.
// When the client chose Innos, the host's hero stayed in chapter 1 (storyinnosdiag, 25.10.).
constexpr char ChapterEntry[] = "G3T_Chapter";
bool chapterSharing = true;  // QA negative control: chaptersyncoff
bool EventsReady() { return Running() && gCSession::GetInstance().GetHero(); }

void EventSnapshot(States& states)
{
    for (auto& entry : everSeen) entry.second = 0;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    auto* memory = static_cast<gCPlayerMemory_PS*>(hero->GetPropertySet(eEPropertySetType_PlayerMemory));
    if (memory) {
        auto& knows = memory->AccessPlayerKnows();
        for (int i = 0; i < knows.GetCount(); ++i) everSeen[knows.AccessAt(i).GetText()] = 1;
    }
    for (auto const& entry : everSeen) states[entry.first] = entry.second;
    if (chapterSharing) states[ChapterEntry] = static_cast<uint8_t>(Entity(hero).PlayerMemory.Chapter);
}

bool EventApply(char const* name, uint32_t known)
{
    Entity hero(gCSession::GetInstance().GetHero());
    if (!std::strcmp(name, ChapterEntry)) {
        if (!chapterSharing) return true;
        if (known && static_cast<uint32_t>(hero.PlayerMemory.Chapter) != known) Logf("story: chapter %d -> %u", static_cast<int>(hero.PlayerMemory.Chapter), known);
        if (known) hero.PlayerMemory.Chapter = known;
        return true;
    }
    const bCString event(name);
    if (known) hero.PlayerMemory.SetGameEvent(event);
    else hero.PlayerMemory.ClearGameEvent(event);
    everSeen[name] = known ? 1 : 0;
    return true;
}

bool ClockReady() { net::ClockState state{}; return clock::CaptureAdmission(state); }
void ClockSnapshot(States& states)
{
    net::ClockState state{};
    if (clock::CaptureAdmission(state)) states[policy::ClockText(state)] = 1;
}
bool ClockApply(char const* name, uint32_t value)
{
    if (!value) return true; // A cleared older date is not a second clock update.
    net::ClockState state{};
    return value == 1 && policy::ReadClock(name, state) && clock::RestoreAdmission(state);
}
Domain domains[] = {
    {net::StateQuests, "quests", &QuestsReady, &QuestSnapshot, &QuestApply},
    {net::StateInfos, "infos", &InfosReady, &InfoSnapshot, &InfoApply},
    {net::StateEvents, "events", &EventsReady, &EventSnapshot, &EventApply},
    {net::StateQuestRuntime, "quest runtime", &QuestsReady, &QuestRuntimeSnapshot, &QuestRuntimeApply},
    {net::StateWorldClock, "world clock", &ClockReady, &ClockSnapshot, &ClockApply},
};

Domain* Find(uint8_t kind)
{
    for (Domain& d : domains)
        if (d.kind == kind) return &d;
    return nullptr;
}

bool BuildNames()
{
    if (namesReady) return true;
    if (!QuestsReady() || !InfosReady()) return false;
    questNames.clear(); infoNames.clear();
    auto add = [](auto& names, char const* name) {
        const std::string id = policy::NameId(name);
        const auto found = names.find(id);
        if (found != names.end() && found->second != name) {
            Logf("story: name identity collision id=%s; admission refused", id.c_str()); return false;
        }
        names[id] = name;
        return true;
    };
    auto& quests = Quests()->AccessQuests();
    auto& infos = Infos()->AccessInfos();
    if (!quests.GetCount() || !infos.GetCount()) return false;
    for (int i = 0; i < quests.GetCount(); ++i)
        if (auto* q = static_cast<gCQuest_PS*>(quests.AccessAt(i)); q && !add(questNames, q->AccessName().GetText())) return false;
    for (int i = 0; i < infos.GetCount(); ++i)
        if (auto* info = infos.AccessAt(i); info && !add(infoNames, info->AccessName().GetText())) return false;
    namesReady = true;
    return true;
}

bool Send(Domain& d, std::vector<net::StateEntry> const& entries, uint8_t to = 0, bool snapshot = false, bool catchup = false)
{
    const uint8_t kind = d.kind | (snapshot ? policy::Snapshot : 0) | (catchup ? policy::Catchup : 0);
    auto ordered = entries;
    if (d.kind == net::StateQuestRuntime)
        std::stable_sort(ordered.begin(), ordered.end(), [](auto const& a, auto const& b) {
            auto size = [](auto const& entry) {
                std::string key, quest, field;
                uint32_t value = 0;
                return policy::UnpackRuntime(entry.name, key, value) && policy::RuntimeKey(key, quest, field) && field == "n";
            };
            return size(a) && !size(b);
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
        std::string quest, field;
        if (!policy::RuntimeKey(name, quest, field) ||
            !policy::PackRuntime(policy::NameId(quest) + '\x1f' + field, value, sizeof(entry.name), packed)) {
            Logf("story: quest runtime cannot be encoded name=%s; snapshot refused", name.c_str());
            ++d.skipped; return false;
        }
        entry.value = 1;
    } else if (d.kind == net::StateQuests || d.kind == net::StateInfos) packed = policy::NameId(name);
    if (packed.size() >= sizeof(entry.name)) {
        Logf("story: named state too long kind=%u name=%s; snapshot refused", d.kind, name.c_str());
        ++d.skipped; return false;
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

bool CommitBaselines()
{
    for (Domain const& d : domains) if (!d.baseline.complete || !d.ready()) return false;
    Domain& runtime = *Find(net::StateQuestRuntime);
    for (auto const& entry : runtime.baseline.values) {
        std::string quest, field;
        uint32_t index = 0;
        if (policy::RuntimeKey(entry.first, quest, field) && field[0] == 'c' &&
            policy::Number(field.substr(1), index)) {
            const auto count = runtime.baseline.values.find(quest + '\x1f' + "n");
            if (count == runtime.baseline.values.end() || index >= count->second) {
                Logf("story: contradictory quest counter baseline name=%s; admission retry required", quest.c_str());
                runtime.baseline = {}; runtime.nextRequest = 0;
                return false;
            }
        }
    }
    // Clock and runtime precede statuses; no Running quest evaluates against the fresh world's date.
    for (Domain& d : domains) {
        States current;
        d.snapshot(current);
        for (auto const& entry : current) {
            if (d.kind == net::StateQuestRuntime) {
                std::string quest, field;
                uint32_t index = 0;
                if (policy::RuntimeKey(entry.first, quest, field) && field[0] == 'c' &&
                    policy::Number(field.substr(1), index)) {
                    const auto length = d.baseline.values.find(quest + '\x1f' + "n");
                    if (length == d.baseline.values.end() || index >= length->second) continue;
                }
            }
            if (!d.baseline.values.count(entry.first)) d.baseline.values[entry.first] = 0;
        }
    }
    for (auto const& entry : Find(net::StateWorldClock)->baseline.values)
        if (entry.second && !ClockApply(entry.first.c_str(), entry.second)) return false;
    for (int phase = 0; phase < 2; ++phase)
        for (auto const& entry : runtime.baseline.values)
            if ((entry.first.back() == 'n') == (phase == 0)) Apply(runtime, entry.first.c_str(), entry.second, true);
    for (Domain& d : domains) {
        if (d.kind != net::StateQuestRuntime && d.kind != net::StateWorldClock)
            for (auto const& entry : d.baseline.values) Apply(d, entry.first.c_str(), entry.second, true);
        d.known.clear();
        for (auto const& entry : d.baseline.values) d.known[entry.first] = entry.second;
        d.primed = true; d.wasConnected = true;
        Logf("story: authoritative %s baseline restored entries=%zu rewards=0", d.label, d.known.size());
    }
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
        if (!net::InWorld(1) || !net::PeerJoined(from)) return;
        // A client entered the game (or reconnected): everything off its default, so it continues the
        // host's story. Existing peers already know it.
        auto& replay = initialReplays[from];
        const uint32_t generation = net::PeerGeneration(from);
        if (replay.generation != generation) { replay = {}; replay.generation = generation; }
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
        return;
    }
    if ((net::IsHost() && snapshot) || (!net::IsHost() && (!admissionRequested || (snapshot && !catchup && d->baseline.complete)))) return;
    if (!net::IsHost() && snapshot && !count) {
        if (!end) { d->baseline.Begin(); return; }
        if (!d->baseline.Finish()) return;
        CommitBaselines();
        return;
    }
    std::vector<std::pair<std::string, uint32_t>> decoded;
    auto reject = [&]() {
        if (!net::IsHost() && snapshot) { d->baseline = {}; d->nextRequest = 0; }
    };
    for (int i = 0; i < count; ++i) {
        std::string name = entries[i].name;
        uint32_t value = entries[i].value;
        if (d->kind == net::StateQuestRuntime &&
            (entries[i].value != 1 || !policy::UnpackRuntime(name, name, value))) {
            Logf("story: malformed quest runtime from player=%u; ignored", from); reject(); return;
        }
        if (d->kind == net::StateWorldClock) {
            net::ClockState state{};
            if (!snapshot || value > 1 || !policy::ReadClock(name, state)) {
                Log("story: invalid bootstrap clock; admission retry required"); reject(); return;
            }
        }
        if (d->kind != net::StateEvents && d->kind != net::StateWorldClock) {
            if (!BuildNames()) { reject(); return; }
            std::string id = name, field;
            if (d->kind == net::StateQuestRuntime && !policy::RuntimeKey(name, id, field)) return;
            auto const& names = d->kind == net::StateInfos ? infoNames : questNames;
            auto found = names.find(id);
            if (found == names.end()) {
                Logf("story: unknown named-state identity=%s kind=%u; snapshot refused", id.c_str(), d->kind); reject(); return;
            }
            name = found->second;
            if (d->kind == net::StateQuestRuntime) {
                if ((field == "n" && value > 512) || (field == "h" && value > 23)) {
                    Logf("story: invalid quest runtime field=%s value=%u; snapshot refused", field.c_str(), value); reject(); return;
                }
                name += '\x1f' + field;
            }
        }
        if (d->kind == net::StateQuests && value > gEQuestStatus_Won) {
            Logf("story: invalid quest status from player=%u; ignored", from); reject(); return;
        }
        if (d->kind == net::StateInfos && value > 1) {
            Logf("story: invalid dialog-state flag from player=%u; ignored", from); reject(); return;
        }
        decoded.emplace_back(std::move(name), value);
    }
    if (!net::IsHost() && snapshot && !catchup) {
        for (auto const& entry : decoded)
            if (!d->baseline.Add(entry.first, entry.second)) return;
        return;
    }
    if (!net::IsHost() && !admissionApplied) {
        if (d->baseline.begun)
            for (auto const& entry : decoded) d->baseline.values[entry.first] = entry.second;
        return;
    }
    if (net::IsHost()) {
        // A client changed these: apply, then tell everyone the host's resulting values.
        for (auto const& entry : decoded) Apply(*d, entry.first.c_str(), entry.second);
        States current;
        d->snapshot(current);
        std::vector<net::StateEntry> result;
        for (auto const& entry : decoded) {
            auto it = current.find(entry.first);
            if (it == current.end()) continue;
            Track(*d, it->first, it->second);
            d->known[it->first] = it->second;
            Entry(*d, it->first, it->second, result);
        }
        Send(*d, result);
        Logf("story: applied %d %s change(s) from player %u", count, d->label, from);
    } else {
        for (auto const& entry : decoded) {
            if (d->kind == net::StateWorldClock && !ClockApply(entry.first.c_str(), entry.second)) {
                admissionApplied = false; d->baseline = {}; d->nextRequest = 0; return;
            }
            d->known[entry.first] = entry.second;
            if (d->kind != net::StateWorldClock) Apply(*d, entry.first.c_str(), entry.second, catchup);
        }
    }
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
        Logf("story: local %s %s=%d", d.label, name.c_str(), static_cast<int>(value));
        Track(d, name, value);
        d.known[name] = value;
        Entry(d, name, value, changes);
    }
    if (changes.empty()) return;
    if (!net::IsHost()) d.claims += static_cast<unsigned>(changes.size());
    Send(d, changes);
}
}

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
bool FinishAdmission(unsigned char player)
{
    if (!net::IsHost() || player < 2 || player >= net::MaxPlayers) return false;
    auto& replay = initialReplays[player];
    if (!replay.captured || replay.sent != 0x1f || replay.generation != net::PeerGeneration(player)) return false;
    std::array<States, 5> captured;
    for (Domain& domain : domains) {
        if (!domain.ready()) return false;
        domain.snapshot(captured[domain.kind - 1]);
    }
    std::unordered_set<std::string> changedQuests;
    for (auto const& entry : policy::Changes(replay.latest[net::StateQuests - 1], captured[net::StateQuests - 1]))
        changedQuests.insert(entry.first);
    for (uint8_t kind : {net::StateWorldClock, net::StateQuestRuntime, net::StateQuests, net::StateInfos, net::StateEvents}) {
        Domain& d = *Find(kind);
        States& current = captured[d.kind - 1];
        States delta = policy::Changes(replay.latest[d.kind - 1], current);
        if (kind == net::StateQuestRuntime) {
            for (auto const& entry : delta) {
                std::string quest, field;
                if (policy::RuntimeKey(entry.first, quest, field)) changedQuests.insert(quest);
            }
            // Resizing a delivery counter requires its full row, not just the one changed element.
            for (auto const& entry : current) {
                std::string quest, field;
                if (policy::RuntimeKey(entry.first, quest, field) && changedQuests.count(quest)) delta[entry.first] = entry.second;
            }
        }
        std::vector<net::StateEntry> changes;
        for (auto const& entry : delta) {
            if (d.kind == net::StateQuestRuntime) {
                std::string quest, field;
                uint32_t index = 0;
                if (policy::RuntimeKey(entry.first, quest, field) && field[0] == 'c' && policy::Number(field.substr(1), index)) {
                    const auto size = current.find(quest + '\x1f' + "n");
                    if (size == current.end() || index >= size->second) continue;
                }
            }
            if (!Entry(d, entry.first, entry.second, changes)) return false;
        }
        if (!Send(d, changes, player, true, true)) return false;
        replay.latest[d.kind - 1] = std::move(current);
    }
    return true;
}
// The client's known values were the host's as far as it knew them in the world before. After a load (the host's save,
// or its own) the first poll took every difference to the loaded save for its own player's changes and sent them; the
// host applied them, and what it changed during the client's load went back to the save's value (review 26.10.: a quest
// back to running, a told dialog option offered again). Primed anew, the client takes the loaded state as it is, as at
// its first join, and asks for the host's full state.
void OnWorldChange()
{
    namesReady = false;
    initialReplays = {};
    if (!reloadReset || net::IsHost()) return;
    admissionRequested = false;
    admissionApplied = false;
    for (Domain& d : domains) {
        d.known.clear();
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
    for (Domain& d : domains) Poll(d);
}

// QA: the local hero's knowledge of a story event; set or clear it as a dialog would.
void Event(char const* action, char const* name, LogFn log)
{
    char line[200];
    if (!EventsReady()) { log("REFUSED event: no hero"); return; }
    Entity hero(gCSession::GetInstance().GetHero());
    const bCString event(name);
    if (!std::strcmp(action, "setevent")) hero.PlayerMemory.SetGameEvent(event);
    else if (!std::strcmp(action, "clearevent")) hero.PlayerMemory.ClearGameEvent(event);
    Format(line, "EVENT name=%s known=%d", name, hero.PlayerMemory.IsSetGameEvent(event) ? 1 : 0);
    log(line);
}

void Report(LogFn log)
{
    char line[260];
    for (Domain const& d : domains) {
        size_t longest = 0;
        for (auto const& entry : d.known) longest = entry.first.size() > longest ? entry.first.size() : longest;
        Format(line, "STORY kind=%s longest=%zu disabled=%d primed=%d known=%zu sent=%u received=%u applied=%u forced=%u claims=%u requests=%u skipped=%u zeroed=%zu reloads=%u",
                  d.label, longest, disabled, d.primed, d.known.size(), d.sent, d.received, d.applied, d.forced, d.claims, d.requests, d.skipped,
                  d.zeroed.size(), reloads);
        log(line);
    }
    log("STORY end");
}

void ChapterOff(LogFn log)
{
    chapterSharing = false;
    log("CHAPTERSYNCOFF ok=1");
}

void Disable(LogFn log)
{
    disabled = true;
    log("STORYOFF ok=1");
}

void ZerosOff(LogFn log)
{
    fullZeros = false;
    log("STORYZEROSOFF ok=1 (the full state leaves out what was cleared)");
}

void ReloadOff(LogFn log)
{
    reloadReset = false;
    log("STORYRELOADOFF ok=1");
}

void Info(char const* name, bool give, LogFn log)
{
    char line[260];
    gCInfoManager_PS* manager = Infos();
    gCInfo_PS* info = manager ? manager->GetInfo(bCString(name)) : nullptr;
    if (!info) { Format(line, "INFO name=%s missing=1", name); log(line); return; }
    if (give) info->AccessInfoGiven() = true;
    Format(line, "INFO name=%s given=%d permanent=%d owner=%s", name, info->AccessInfoGiven() ? 1 : 0,
              info->AccessPermanent() ? 1 : 0, info->AccessOwner().GetText());
    log(line);
}

void InfoCandidates(LogFn log)
{
    char line[260];
    gCInfoManager_PS* manager = Infos();
    int found = 0;
    if (manager) {
        auto& all = manager->AccessInfos();
        for (int i = 0; i < all.GetCount() && found < 20; ++i) {
            gCInfo_PS* info = all.AccessAt(i);
            if (!info || info->AccessInfoGiven() || info->AccessPermanent() || info->AccessOwner().IsEmpty()) continue;
            char const* name = info->AccessName().GetText();
            // Names the QA command file accepts: [A-Za-z0-9_-], and short enough for one message entry.
            bool plain = std::strlen(name) < sizeof(net::StateEntry::name);
            for (char const* c = name; *c && plain; ++c) plain = isalnum(static_cast<unsigned char>(*c)) || *c == '_' || *c == '-';
            if (!plain) continue;
            Format(line, "INFOS name=%s owner=%s", name, info->AccessOwner().GetText());
            log(line);
            ++found;
        }
        Format(line, "INFOS total=%d", all.GetCount());
        log(line);
    }
    log("INFOS end");
}
}

namespace g3t::story {
// QA diagnostics: hold a dialog the way talking to an NPC does (info manager between hero and NPC).
void Talk(char const* npcName, LogFn log)
{
    gCInfoManager_PS* manager = Infos();
    eCEntity* hero = gCSession::GetInstance().GetHero();
    Entity npc = Named(npcName);
    if (!manager || !hero || !npc.GetInstance()) { log("REFUSED talk: no game, hero or NPC"); return; }
    const GEBool started = manager->StartInfoManager(*hero, *npc.GetInstance());
    // The options on offer; ":xp" marks one whose script gives experience (GiveXP), for stage Q2.
    std::string offered;
    for (GEInt i = 0; i < manager->GetAvailableInfoCount(); ++i) {
        gCInfo_PS* info = manager->GetAvailableInfo(i);
        if (!info) continue;
        bool xp = false;
        gCInfoScript_PS* script = info->AccessInfoScript().GetObject();
        for (GEInt c = 0; script && c < script->GetCommandCount(); ++c)
            if (gCInfoScriptLine_PS* command = script->GetCommand(c)) xp = xp || command->AccessCommand() == bCString("GiveXP");
        offered += std::string(offered.empty() ? "" : ",") + info->AccessName().GetText() + (xp ? ":xp" : "");
    }
    char line[900];
    Format(line, "TALK npc=%s started=%d running=%d available=%d npc_task=%s infos=%s", npcName, started ? 1 : 0,
              manager->AccessIsRunning() ? 1 : 0, manager->GetAvailableInfoCount(), npc.Routine.GetCurrentTask().GetText(),
              offered.empty() ? "-" : offered.c_str());
    log(line);
}

void TalkState(LogFn log)
{
    char line[260];
    gCInfoManager_PS* manager = Infos();
    if (!manager) { log("REFUSED talkstate: no game"); return; }
    gCInfo_PS* current = manager->GetCurrentInfo();
    gCEntity* partner = manager->GetDialogPartnerNPC();
    Format(line, "TALKSTATE running=%d available=%d current=%s partner=%s waiting=%d", manager->AccessIsRunning() ? 1 : 0,
              manager->GetAvailableInfoCount(), current ? current->AccessName().GetText() : "-",
              partner ? partner->GetName().GetText() : "-", manager->AccessWaitForScript() ? 1 : 0);
    log(line);
}

void EndTalk(LogFn log)
{
    gCInfoManager_PS* manager = Infos();
    if (!manager) { log("REFUSED endtalk: no game"); return; }
    manager->EndInfoManager(manager->GetCurrentInfo());
    TalkState(log);
}
}

namespace g3t::story {
// QA: address an NPC the way the player's talk action does (the AssessTalk script, NPC first).
void AssessTalk(char const* npcName, LogFn log)
{
    char line[260];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    Entity npc = Named(npcName);
    if (!hero || !npc.GetInstance()) { log("REFUSED assesstalk: no hero or NPC"); return; }
    const GEInt result = GetScriptAdminExt().CallScript(bCString("AssessTalk"), npc.GetInstance(), hero, 0);
    Format(line, "ASSESSTALK npc=%s result=%d npc_task=%s npc_aimode=%d hero_task=%s has_info=%d frozen=%d npc_talking=%d npc_talked_to=%d",
              npcName, result, npc.Routine.GetCurrentTask().GetText(), body::AIModeOf(npc.GetInstance()),
              Entity(hero).Routine.GetCurrentTask().GetText(), npc.HasInfo(Entity(hero)) ? 1 : 0, npc.NPC.IsFrozen() ? 1 : 0,
              npc.Dialog.IsTalking() ? 1 : 0, npc.Dialog.IsBeingTalkedTo() ? 1 : 0);
    log(line);
}

// QA (host): end the opening raid so that the people of Ardea calm down and can talk.
void KillRaiders(LogFn log)
{
    char line[120];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    int killed = 0;
    const bTObjArray<Entity> all = Entity::GetNPCs();
    for (int i = 0; i < all.GetCount() && hero; ++i) {
        Entity npc = all.GetAt(i);
        if (!npc.GetInstance() || npc.IsDead() || std::strncmp(npc.GetName().GetText(), "Orc_", 4) != 0) continue;
        const bCVector& a = npc.GetInstance()->GetWorldPosition();
        const bCVector& b = hero->GetWorldPosition();
        const float at[3] = {a.GetX(), a.GetY(), a.GetZ()}, from[3] = {b.GetX(), b.GetY(), b.GetZ()};
        if (body::Distance(at, from) > 30000.0f) continue;
        // Not Entity::Kill (removes the body while others still target it: crash) and not a death task set
        // from outside (crashed in Script.dll mid-fight): a lethal hit, judged by the game.
        if (combat::LethalHit(npc.GetInstance())) ++killed;
    }
    Format(line, "KILLRAIDERS killed=%d", killed);
    log(line);
}
}

namespace g3t::story {
// QA: put the hero 1.5 m from an NPC, on the side he came from, as a player walks up before talking.
void Approach(char const* npcName, LogFn log)
{
    char line[200];
    eCDynamicEntity* hero = gCSession::GetInstance().GetHero();
    Entity npc = Named(npcName);
    if (!hero || !npc.GetInstance()) { log("REFUSED approach: no hero or NPC"); return; }
    const bCVector& n = npc.GetInstance()->GetWorldPosition();
    const bCVector& h = hero->GetWorldPosition();
    const float target[3] = {n.GetX(), n.GetY(), n.GetZ()}, from[3] = {h.GetX(), h.GetY(), h.GetZ()};
    const float before = body::Distance(target, from);
    const float dx = before > 1.0f ? (from[0] - target[0]) / before : 1.0f, dz = before > 1.0f ? (from[2] - target[2]) / before : 0.0f;
    const float position[3] = {target[0] + 150.0f * dx, target[1], target[2] + 150.0f * dz};
    const bCQuaternion q = hero->GetWorldRotation();
    const float rotation[4] = {q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
    Entity(hero).MoveTo(body::Pose(*hero, position, rotation));
    const bCVector& p = hero->GetWorldPosition();
    const float now[3] = {p.GetX(), p.GetY(), p.GetZ()};
    Format(line, "APPROACH npc=%s before_cm=%.0f after_cm=%.0f", npcName, before, body::Distance(target, now));
    log(line);
}
}

namespace g3t::story {
// QA: NPCs within `radius` cm (default 2000, the distance from which AssessActivePerception lets an NPC with an important
// info walk up to the player and talk) that have an important info for this machine's hero, as the game asks it.
void Important(char const* radius, LogFn log)
{
    char line[300];
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero) { log("REFUSED important: no hero"); return; }
    const float within = radius && *radius ? static_cast<float>(std::atof(radius)) : 2000.0f;
    const bCVector& h = hero->GetWorldPosition();
    const float from[3] = {h.GetX(), h.GetY(), h.GetZ()};
    const bTObjArray<Entity> list = Entity::GetNPCs();
    for (int i = 0; i < list.GetCount(); ++i) {
        Entity npc = list.GetAt(i);
        if (!npc.GetInstance() || npc.IsDead() || remote::IsPuppet(npc.GetInstance())) continue;
        const bCVector& n = npc.GetInstance()->GetWorldPosition();
        const float at[3] = {n.GetX(), n.GetY(), n.GetZ()};
        const float distance = body::Distance(at, from);
        if (distance > within || !npc.HasImportantInfo(Entity(hero))) continue;
        Format(line, "IMPORTANT npc=%s distance=%.0f mirrored=%d task=%s", npc.GetName().GetText(), distance,
                  npcs::IsMirrored(npc.GetInstance()) ? 1 : 0, npc.Routine.GetCurrentTask().GetText());
        log(line);
    }
    log("IMPORTANT end");
}
}
