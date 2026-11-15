// SPDX-License-Identifier: GPL-3.0-or-later
#include "story_internal.h"

#include "clock.h"
#include "enclaves.h"
#include "log.h"
#include "reputation_policy.h"

#include <cstring>

namespace g3t::story::detail {
namespace {
// Dialogs set story events on the hero who holds the dialog (SetGameEvent / ClearGameEvent, kept in the hero's
// PlayerMemory.PlayerKnows); other dialogs check them. Shared like quests, so that every hero knows what one
// learned (the client's Diego dialog set Diego_WarIsLost on the client's hero only; stage H exploration).
std::unordered_map<std::string, uint8_t> everSeen;  // events that were set at some point: 0 once cleared
bool namesReady = false;
std::unordered_map<std::string, std::string> questNames;
std::unordered_map<std::string, std::string> infoNames;
std::unordered_map<std::string, uint32_t> seenInfoEnds;

bool QuestsReady() { return Quests() != nullptr; }

void QuestSnapshot(States& states)
{
    auto& all = Quests()->AccessQuests();
    for (int i = 0; i < all.GetCount(); ++i)
        if (gCQuest_PS* quest = static_cast<gCQuest_PS*>(all.AccessAt(i))) states[quest->AccessName().GetText()] = StatusOf(quest);
}

gCPlayerMemory_PS* HeroMemory()
{
    eCEntity* hero = Running() ? gCSession::GetInstance().GetHero() : nullptr;
    return hero ? static_cast<gCPlayerMemory_PS*>(hero->GetPropertySet(eEPropertySetType_PlayerMemory)) : nullptr;
}

bool ReputationReady() { return HeroMemory() != nullptr && PSEnclave::GetEnclaveCount() > 0; }

void ReputationSnapshot(States& states)
{
    auto* memory = HeroMemory();
    if (!memory) return;
    auto const& pol = memory->AccessPoliticalFame();
    for (size_t i = 0; i < reputation::PoliticalAlignmentCount; ++i) {
        const int32_t fame = static_cast<GEInt>(i) < pol.GetCount() ? pol.GetAt(static_cast<GEInt>(i)) : 0;
        states[reputation::PoliticalKey(i)] = reputation::EncodeFame(fame);
    }
    const GEInt count = PSEnclave::GetEnclaveCount();
    for (GEInt i = 0; i < count; ++i) {
        Entity e = PSEnclave::GetEnclaveAt(i);
        eCEntity* inst = e.GetInstance();
        auto* enc = inst ? static_cast<gCEnclave_PS*>(inst->GetPropertySet(eEPropertySetType_Enclave)) : nullptr;
        if (!enc) continue;
        char const* rawName = e.GetName().GetText();
        if (rawName && *rawName && std::strlen(rawName) <= 31)
            states[reputation::EnclaveKey(rawName)] = reputation::EncodeFame(enc->AccessPlayerFame());
    }
}

bool ReputationApply(char const* name, uint32_t encoded)
{
    bool isPolitical = false;
    size_t alignment = 0;
    std::string enclaveName;
    if (!name || !reputation::ParseKey(name, isPolitical, alignment, enclaveName)) return false;
    const int32_t fame = reputation::DecodeFame(encoded);
    if (isPolitical) {
        auto* memory = HeroMemory();
        if (!memory) return false;
        auto& pol = memory->AccessPoliticalFame();
        const GEInt oldCount = pol.GetCount();
        const GEInt needed = static_cast<GEInt>(
            reputation::RequiredPoliticalArrayCount(oldCount > 0 ? static_cast<size_t>(oldCount) : 0u, alignment, fame));
        if (oldCount < needed) {
            pol.SetCount(needed);
            for (GEInt i = oldCount < 0 ? 0 : oldCount; i < needed; ++i) pol.AccessAt(i) = 0;
        }
        if (static_cast<GEInt>(alignment) < pol.GetCount()) pol.AccessAt(static_cast<GEInt>(alignment)) = fame;
        return true;
    }
    Entity e = PSEnclave::GetEnclave(bCString(enclaveName.c_str()));
    eCEntity* inst = e.GetInstance();
    auto* enc = inst ? static_cast<gCEnclave_PS*>(inst->GetPropertySet(eEPropertySetType_Enclave)) : nullptr;
    if (!enc) return false;
    enc->AccessPlayerFame() = fame;
    enclaves::NoteAuthoritativeFame(enclaveName.c_str(), fame);
    return true;
}

// Through the game's own functions where one exists (journal entries, experience), else set directly.
bool QuestApply(char const* name, uint32_t status)
{
    gCQuestManager_PS* manager = Quests();
    const bCString id(name);
    gCQuest_PS* quest = manager->GetQuest(id);
    if (!quest || StatusOf(quest) == status) return true;
    States preservedRep;
    const bool preserveRep = !net::IsHost() && ReputationReady();
    if (preserveRep) ReputationSnapshot(preservedRep);
    const gEQuestStatus target = static_cast<gEQuestStatus>(status);
    if ((target == gEQuestStatus_Success || target == gEQuestStatus_Failed) && StatusOf(quest) == gEQuestStatus_Open)
        manager->RunQuest(id);
    if (target == gEQuestStatus_Running) manager->RunQuest(id);
    else if (target == gEQuestStatus_Success) manager->SucceedQuest(id);
    else if (target == gEQuestStatus_Failed) manager->FailQuest(id);
    const bool nativeHandled = StatusOf(quest) == status;
    if (!nativeHandled) quest->SetStatus(target);
    if (preserveRep) {
        for (auto const& [repKey, repVal] : preservedRep) ReputationApply(repKey.c_str(), repVal);
    }
    return nativeHandled;
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
    for (auto& entry : seenInfoEnds) entry.second = 0;
    if (auto* mgr = Infos()) {
        auto& infos = mgr->AccessInfos();
        for (int i = 0; i < infos.GetCount(); ++i)
            if (auto* info = infos.AccessAt(i); info && info->m_uEndTimestamp)
                seenInfoEnds[std::string(info->AccessName().GetText()) + "\x1fi"] = info->m_uEndTimestamp;
    }
    for (auto const& entry : seenInfoEnds) states[entry.first] = entry.second;
}

bool QuestRuntimeApply(char const* name, uint32_t value)
{
    policy::ParsedRuntimeKey parsed;
    if (!name || !policy::ParseRuntimeKey(name, parsed)) return false;
    if (parsed.field == policy::RuntimeField::InfoEnd) {
        auto* mgr = Infos();
        auto* info = mgr ? mgr->GetInfo(bCString(parsed.target.c_str())) : nullptr;
        if (!info) return false;
        info->m_uEndTimestamp = value;
        seenInfoEnds[name] = value;
        return true;
    }
    auto* quest = Quests()->GetQuest(bCString(parsed.target.c_str()));
    if (!quest) return false;
    switch (parsed.field) {
    case policy::RuntimeField::Year: quest->AccessActivationTime_Year() = value; return true;
    case policy::RuntimeField::Day: quest->AccessActivationTime_Day() = value; return true;
    case policy::RuntimeField::Hour: quest->AccessActivationTime_Hour() = value; return true;
    case policy::RuntimeField::CounterCount: {
        if (value > 512) return false;
        auto& counters = quest->AccessDeliveryCounter();
        const int previous = counters.GetCount();
        counters.SetCount(static_cast<int>(value));
        for (int i = previous; i < counters.GetCount(); ++i) counters.AccessAt(i) = 0;
        return true;
    }
    case policy::RuntimeField::CounterElem: {
        auto& counters = quest->AccessDeliveryCounter();
        if (parsed.index >= static_cast<uint32_t>(counters.GetCount())) return false;
        counters.AccessAt(static_cast<int>(parsed.index)) = static_cast<GEI32>(value);
        return true;
    }
    case policy::RuntimeField::InfoEnd:
        break;
    }
    return false;
}

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

// The chapter lives beside story events in the hero's PlayerMemory (SetChapter, info command 53: 2 once a god is chosen) and decides
// attitudes through Entity::GetPlayer (re/out/notify-enclave-predicate-20261024-c.c): shared as one more entry of this kind.
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
        if (known && static_cast<uint32_t>(hero.PlayerMemory.Chapter) != known)
            Logf("story: chapter %d -> %u", static_cast<int>(hero.PlayerMemory.Chapter), known);
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
bool BuildNames()
{
    if (namesReady) return true;
    if (!QuestsReady() || !InfosReady()) return false;
    questNames.clear();
    infoNames.clear();
    auto add = [](auto& names, char const* name) {
        const std::string id = policy::NameId(name);
        const auto found = names.find(id);
        if (found != names.end() && found->second != name) {
            Logf("story: name identity collision id=%s; admission refused", id.c_str());
            return false;
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
} // namespace

bool chapterSharing = true;

bool Running() { return gCSession::GetInstance().IsGameRunning(); }
gCQuestManager_PS* Quests() { return Running() ? gCQuestManager_PS::GetInstance() : nullptr; }
uint8_t StatusOf(gCQuest_PS* quest) { return static_cast<uint8_t>(static_cast<gEQuestStatus const&>(quest->AccessStatus())); }
gCInfoManager_PS* Infos() { return Running() ? gCInfoManager_PS::GetInstance() : nullptr; }
bool EventsReady() { return Running() && gCSession::GetInstance().GetHero(); }

bool ClockApply(char const* name, uint32_t value)
{
    if (!value) return true; // A cleared older date is not a second clock update.
    net::ClockState state{};
    return value == 1 && policy::ReadClock(name, state) && clock::RestoreAdmission(state);
}

Domain domains[DomainCount] = {
    {net::StateQuests, "quests", &QuestsReady, &QuestSnapshot, &QuestApply},
    {net::StateInfos, "infos", &InfosReady, &InfoSnapshot, &InfoApply},
    {net::StateEvents, "events", &EventsReady, &EventSnapshot, &EventApply},
    {net::StateQuestRuntime, "quest runtime", &QuestsReady, &QuestRuntimeSnapshot, &QuestRuntimeApply},
    {net::StateWorldClock, "world clock", &ClockReady, &ClockSnapshot, &ClockApply},
    {net::StateReputation, "reputation", &ReputationReady, &ReputationSnapshot, &ReputationApply},
};

Domain* Find(uint8_t kind)
{
    for (Domain& d : domains)
        if (d.kind == kind) return &d;
    return nullptr;
}

void ResetDomainCaches()
{
    namesReady = false;
    questNames.clear();
    infoNames.clear();
    seenInfoEnds.clear();
    everSeen.clear();
}

bool ResolveNamedState(uint8_t domainKind, std::string& ioName, uint32_t value)
{
    if (!BuildNames()) return false;
    policy::ParsedRuntimeKey parsed;
    std::string id = ioName;
    bool useInfos = domainKind == net::StateInfos;
    if (domainKind == net::StateQuestRuntime) {
        if (!policy::ParseRuntimeKey(ioName, parsed)) return false;
        id = parsed.target;
        useInfos = parsed.field == policy::RuntimeField::InfoEnd;
    }
    auto const& names = useInfos ? infoNames : questNames;
    const auto found = names.find(id);
    if (found == names.end()) {
        Logf("story: unknown named-state identity=%s kind=%u; snapshot refused", id.c_str(), domainKind);
        return false;
    }
    if (domainKind == net::StateQuestRuntime) {
        if ((parsed.field == policy::RuntimeField::CounterCount && value > 512) ||
            (parsed.field == policy::RuntimeField::Hour && value > 23)) {
            Logf("story: invalid quest runtime field=%s value=%u; snapshot refused",
                 policy::FormatRuntimeField(parsed).c_str(), value);
            return false;
        }
        ioName = found->second + '\x1f' + policy::FormatRuntimeField(parsed);
        return true;
    }
    ioName = found->second;
    return true;
}

void Track(Domain& d, std::string const& name, uint32_t value)
{
    if (!net::IsHost()) return;
    auto it = d.known.find(name);
    if (value) d.zeroed.erase(name);
    else if (it != d.known.end() && it->second) d.zeroed.insert(name);
}

} // namespace g3t::story::detail
