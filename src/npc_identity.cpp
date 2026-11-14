// SPDX-License-Identifier: GPL-3.0-or-later
#include "npc_identity.h"
#include "npc_state.h"

#include "log.h"
#include "npclease_presentation.h"
#include "npclease_runtime.h"
#include "npcremoval.h"
#include "talk.h"
#include "world.h"

#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace g3t::npcs {
using namespace detail;

namespace {
npcidentity::Candidate IdentityCandidate(eCEntity* entity)
{
    npcidentity::Candidate candidate;
    if (!entity) return candidate;
    candidate.instance = reinterpret_cast<uintptr_t>(entity);
    Guid(entity, candidate.guid.data());
    std::memcpy(candidate.templateId.data(), &entity->GetTemplateID().GetCore(), 16);
    candidate.name = entity->GetName().GetText();
    if (!IsNpc(entity) || entity->GetPropertySet(eEPropertySetType_Item) ||
        entity->GetPropertySet(eEPropertySetType_Magic) || npcremoval::Suppressed(entity)) return candidate;
    Entity npc(entity);
    if (entity->GetPropertySet(eEPropertySetType_Interaction) &&
        npc.Interaction.GetOwner().GetInstance()) return candidate;
    if (auto* party = static_cast<gCParty_PS*>(entity->GetPropertySet(eEPropertySetType_Party))) {
        if (npc.Party.GetPartyLeader().GetInstance() || npc.Party.Waiting ||
            static_cast<gEPartyMemberType>(npc.Party.PartyMemberType) != gEPartyMemberType_None ||
            party->GetMembers().GetCount()) return candidate;
    }
    const GuidKey key = Key(candidate.guid.data());
    const auto mirror = mirrors.find(key);
    if (mirror != mirrors.end() && (mirror->second.latest.partyPlayer || mirror->second.latest.guidePlayer))
        return candidate;
    if (claimedBy.count(key) || directedBy.count(key) ||
        talk::TalkingWithHero(entity) || talk::IsHeld(entity)) return candidate;
    candidate.protectedRole = false;
    return candidate;
}

bool IndependentActor(eCEntity* entity)
{
    return entity && (entity->GetPropertySet(eEPropertySetType_NPC) ||
        entity->GetPropertySet(eEPropertySetType_PlayerMemory) || remote::IsPuppet(entity) ||
        entity == gCSession::GetInstance().GetHero() || entity == remote::ControlledBody());
}

bool ActorDescendant(eCEntity* entity)
{
    for (int i = 0; i < entity->GetChildrenCount(); ++i)
        if (auto* child = entity->GetChildAt(i); child && (IndependentActor(child) || ActorDescendant(child)))
            return true;
    return false;
}

void QuarantineTree(eCEntity* entity, eCEntity* root)
{
    auto [found, inserted] = quarantinedEntities.try_emplace(entity);
    auto& saved = found->second;
    npcidentity::Guid identity;
    Guid(entity, identity.data());
    if (inserted || saved.guid != identity) {
        saved.guid = identity;
        saved.root = root;
        Guid(root, saved.rootGuid.data());
        saved.rendering = entity->IsRenderingEnabled() != GEFalse;
        saved.picking = entity->IsPickable() != GEFalse;
        saved.collision = entity->GetEntityFlags().m_CollisionEnabled != 0;
        saved.processingDisabled = entity->IsProcessingDisabled() != GEFalse;
    }
    entity->DisableProcessing(GETrue);
    entity->EnableRendering(GEFalse);
    entity->EnablePicking(GEFalse, GEFalse);
    entity->EnableCollision(GEFalse);
    for (int i = 0; i < entity->GetChildrenCount(); ++i)
        if (auto* child = entity->GetChildAt(i); child && !IndependentActor(child)) QuarantineTree(child, root);
}

void LogIdentity(char const* kind, eCEntity* entity, eCEntity* keeper)
{
    const auto candidate = IdentityCandidate(entity);
    const auto mirror = mirrors.find(Key(candidate.guid.data()));
    char id[40], prototype[40];
    guid::Format(candidate.guid.data(), id, sizeof(id));
    guid::Format(candidate.templateId.data(), prototype, sizeof(prototype));
    auto* animation = static_cast<eCVisualAnimation_PS*>(entity->GetPropertySet(eEPropertySetType_Animation));
    const auto* creator = entity->GetCreator();
    auto const& p = entity->GetWorldPosition();
    Logf("npcs: %s guid=%s entity=%p keeper=%p name=%s template=%s template_guid=%s pos=%.1f,%.1f,%.1f "
         "processing=%d rendering=%d bound=%d pose=%d host_age_ms=%u lease=%d fenced=%d protected=%d "
         "actor=%p bodyparts=%d source=%s",
         kind, id, entity, keeper, candidate.name.c_str(), creator ? creator->GetName().GetText() : "(unknown)",
         prototype, p.GetX(), p.GetY(), p.GetZ(), !entity->IsProcessingDisabled(), entity->IsRenderingEnabled(),
         mirror != mirrors.end() && mirror->second.entity.GetInstance() == entity,
         mirror != mirrors.end() && mirror->second.entity.GetInstance() == entity && mirror->second.replayReady,
         HostPresentationAge(entity), npclease::LocalAuthority(entity), npclease::Fenced(entity), candidate.protectedRole,
         animation ? animation->GetActor() : nullptr,
         animation ? animation->GetAnimationFactory().GetAdditionalBodyParts().GetCount() : 0,
         RedundantClientEntity(entity) ? "identity-quarantine" : "native-or-unknown");
    if (animation) {
        auto& factory = animation->GetAnimationFactory();
        Logf("npcs: NPCVISUAL guid=%s entity=%p main=%s facial=%d attachments=%u",
             id, entity, factory.GetMainActorFileName().GetText(), factory.IsFacialAnimationStarted(),
             animation->GetNumAttachments());
        auto const& parts = factory.GetAdditionalBodyParts();
        for (int i = 0; i < parts.GetCount(); ++i)
            Logf("npcs: NPCBODYPART guid=%s entity=%p part=%s visible=%d", id, entity,
                 parts[i].m_strName.GetText(), parts[i].m_bVisible);
    }
}
} // namespace

bool RedundantClientEntity(eCEntity const* entity)
{
    if (!entity || net::IsHost()) return false;
    const auto found = quarantinedEntities.find(entity);
    if (found == quarantinedEntities.end()) return false;
    npcidentity::Guid identity;
    Guid(const_cast<eCEntity*>(entity), identity.data());
    return identity == found->second.guid;
}

void ReconcileClientEntities()
{
    if (net::IsHost() || !gCSession::GetInstance().IsGameRunning() || !gCSession::GetInstance().GetHero()) return;
    const uint32_t now = net::Now();
    if (identityCensusAt && now - identityCensusAt < 250) return;
    identityCensusAt = now ? now : 1;
    const bool report = !identityLogAt || now - identityLogAt >= 5000;
    if (report) identityLogAt = now ? now : 1;
    std::vector<eCEntity*> live, npcs;
    std::unordered_set<uintptr_t> seen;
    auto add = [&](eCEntity* entity) {
        if (npcidentity::AddPointer(seen, reinterpret_cast<uintptr_t>(entity))) live.push_back(entity);
    };
    const auto dynamic = Entity::GetEntities();
    for (int i = 0; i < dynamic.GetCount(); ++i) add(dynamic[i].GetInstance());
    const auto* processed = world::ProcessedEntities();
    for (GEU16 i = 0; processed && i < processed->GetCount(); ++i) add((*processed)[i]);
    for (size_t i = 0; i < live.size(); ++i)
        for (int child = 0; child < live[i]->GetChildrenCount(); ++child) add(live[i]->GetChildAt(child));
    std::unordered_map<GuidKey, std::vector<eCEntity*>, GuidHash> groups;
    std::unordered_map<eCEntity*, npcidentity::Candidate> candidates;
    std::unordered_map<std::string, std::vector<eCEntity*>> names;
    for (auto* entity : live) {
        if (!IsNpc(entity)) continue;
        npcs.push_back(entity);
        auto candidate = IdentityCandidate(entity);
        groups[Key(candidate.guid.data())].push_back(entity);
        if (report) names[candidate.name].push_back(entity);
        candidates.emplace(entity, std::move(candidate));
    }
    std::unordered_set<eCEntity const*> redundant;
    unsigned collisions = 0, unresolved = 0, duplicates = 0;
    for (auto const& entry : groups) {
        if (entry.second.size() < 2) continue;
        ++collisions;
        auto* keeper = Resolve(entry.first).GetInstance();
        const auto canonical = IdentityCandidate(keeper);
        const auto mirror = mirrors.find(entry.first);
        const uintptr_t bound = mirror != mirrors.end() && mirror->second.hostSession == net::WelcomeSerial()
            ? reinterpret_cast<uintptr_t>(mirror->second.entity.GetInstance()) : 0;
        for (auto* entity : entry.second) {
            if (entity == keeper) continue;
            if (npcidentity::Quarantine(candidates.at(entity), canonical, bound) &&
                seen.count(canonical.instance) && !ActorDescendant(entity)) redundant.insert(entity);
            else ++unresolved;
        }
    }
    for (auto* entity : live) {
        auto saved = quarantinedEntities.find(entity);
        if (saved == quarantinedEntities.end()) continue;
        npcidentity::Guid identity;
        Guid(entity, identity.data());
        if (saved->second.guid != identity) { quarantinedEntities.erase(saved); continue; }
        if (npcremoval::Suppressed(saved->second.rootGuid.data())) {
            quarantinedEntities.erase(saved);
            continue;
        }
        if (redundant.count(saved->second.root)) continue;
        entity->DisableProcessing(saved->second.processingDisabled ? GETrue : GEFalse);
        entity->EnableRendering(saved->second.rendering ? GETrue : GEFalse);
        entity->EnablePicking(saved->second.picking ? GETrue : GEFalse, GEFalse);
        entity->EnableCollision(saved->second.collision ? GETrue : GEFalse);
        if (saved->second.root == entity) LogIdentity("NPCDUP_RESTORE", entity, entity);
        quarantinedEntities.erase(saved);
    }
    for (auto it = quarantinedEntities.begin(); it != quarantinedEntities.end();)
        if (!seen.count(reinterpret_cast<uintptr_t>(it->first))) it = quarantinedEntities.erase(it);
        else ++it;
    for (auto* entity : redundant) {
        const bool first = !RedundantClientEntity(entity);
        if (first) LogIdentity("NPCDUP_QUARANTINE", const_cast<eCEntity*>(entity),
                              Resolve(IdentityCandidate(const_cast<eCEntity*>(entity)).guid.data()).GetInstance());
        QuarantineTree(const_cast<eCEntity*>(entity), const_cast<eCEntity*>(entity));
    }
    for (auto const& entry : groups) {
        if (entry.second.size() < 2) continue;
        auto* keeper = Resolve(entry.first).GetInstance();
        for (auto* entity : entry.second) {
            duplicates += entity != keeper && entity->IsRenderingEnabled();
            if (report) LogIdentity("NPCDUP", entity, keeper);
        }
    }
    if (!report) return;
    unsigned nearbyPairs = 0, samples = 0;
    for (auto const& name : names) {
        auto const& entities = name.second;
        for (size_t i = 0; i < entities.size(); ++i) {
            for (size_t j = i + 1; j < entities.size(); ++j) {
                auto const& a = candidates.at(entities[i]);
                auto const& b = candidates.at(entities[j]);
                if (a.guid == b.guid || a.templateId != b.templateId ||
                    (entities[i]->GetWorldPosition() - entities[j]->GetWorldPosition()).GetMagnitude() > 100.0f) continue;
                if (nearbyPairs++ < 8) {
                    LogIdentity("NPCNEAR_DIFFERENT_GUID", entities[i], entities[j]);
                    LogIdentity("NPCNEAR_DIFFERENT_GUID", entities[j], entities[i]);
                }
            }
        }
    }
    for (auto* entity : npcs) {
        if (samples < 16 && (entity->GetWorldPosition() -
            gCSession::GetInstance().GetHero()->GetWorldPosition()).GetMagnitude() <= 1000.0f) {
            LogIdentity("NPCMODEL", entity, nullptr);
            ++samples;
        }
    }
    Logf("npcs: NPCCENSUS npcs=%u guid_collisions=%u duplicates=%u quarantined=%u unresolved=%u "
         "near_name_pairs=%u model_samples=%u",
         static_cast<unsigned>(npcs.size()), collisions, duplicates, static_cast<unsigned>(redundant.size()),
         unresolved, nearbyPairs, samples);
}

namespace detail {

std::string FocusIdentity(eCEntity* entity)
{
    if (!entity) return "null";
    char id[40], text[64];
    guid::FormatEntity(entity, id, sizeof(id));
    std::snprintf(text, sizeof(text), "%p/%s", entity, id);
    return text;
}

bool FocusProcessed(eCEntity* entity)
{
    auto* processed = world::ProcessedEntities();
    if (processed)
        for (GEU16 i = 0; i < processed->GetCount(); ++i)
            if (processed->GetAt(i) == entity) return true;
    return false;
}

std::string FocusRawIdentity(eCEntity* entity)
{
    if (!entity) return "null";
    if (FocusProcessed(entity)) return FocusIdentity(entity);
    char text[48];
    std::snprintf(text, sizeof(text), "%p/not_processed", entity);
    return text;
}

uint32_t FocusEntityFlags(eCEntity* entity)
{
    uint32_t flags = 0;
    static_assert(sizeof(eSEntityFlags) == sizeof(flags));
    std::memcpy(&flags, &entity->GetEntityFlags(), sizeof(flags));
    return flags;
}

Mirror const* FocusMirror(eCEntity* entity)
{
    uint8_t bytes[16];
    guid::CopyEntity(entity, bytes);
    const GuidKey key = Key(bytes);
    const auto mirror = mirrors.find(key);
    if (mirror != mirrors.end() && mirror->second.entity.GetInstance() == entity) return &mirror->second;
    const auto corpse = corpses.find(key);
    return corpse != corpses.end() && corpse->second.entity.GetInstance() == entity ? &corpse->second : nullptr;
}

bool VerifyFocusQueueLayout(unsigned char* module)
{
    if (!module) return false;
    const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(module);
    const auto* pe = reinterpret_cast<IMAGE_NT_HEADERS const*>(module + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || pe->Signature != IMAGE_NT_SIGNATURE ||
        pe->OptionalHeader.SizeOfImage < 0x118d5c) return false;
    auto* countCode = module + 0x7e889;
    auto* dataCode = module + 0x7e89b;
    uintptr_t countAddress = 0, dataAddress = 0;
    std::memcpy(&countAddress, countCode + 2, sizeof(countAddress));
    std::memcpy(&dataAddress, dataCode + 2, sizeof(dataAddress));
    static unsigned char const stride[] = {0x69, 0xc0, 0xb0, 0, 0, 0};
    return countCode[0] == 0x83 && countCode[1] == 0x3d && countCode[6] == 0 &&
        dataCode[0] == 0x8b && dataCode[1] == 0x15 && !std::memcmp(dataCode + 6, "\x83\xc2\x08", 3) &&
        countAddress == reinterpret_cast<uintptr_t>(module + 0x118d58) &&
        dataAddress == reinterpret_cast<uintptr_t>(module + 0x118d54) &&
        !std::memcmp(module + 0xcc4d, stride, sizeof(stride));
}

bool ReadFocusCandidates(uint32_t serial, gCFocus_PS& focus, std::vector<eCEntity*>& candidates)
{
    static const bool supported = [] {
        auto* game = GetModuleHandleA("Game.dll");
        auto* code = game ? reinterpret_cast<unsigned char const*>(GetProcAddress(game,
            "?CollectInteractionEntities@gCFocus_PS@@IAEXM@Z")) : nullptr;
        unsigned char bytes[0x23] = {};
        static unsigned char const array[] = {0x8d, 0xbd, 0x78, 0x01, 0, 0};
        return code && ReadProcessMemory(GetCurrentProcess(), code, bytes, sizeof(bytes), nullptr) &&
            !std::memcmp(bytes + 8, array, sizeof(array)) && !std::memcmp(bytes + 0x20, "\x89\x5f\x04", 3);
    }();
    if (!supported) {
        Logf("npcs: FOCUSSCAN seq=%u cached_layout=unsupported", serial);
        return false;
    }
    struct List { eCEntity** data; int count, capacity; } list{};
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<unsigned char*>(&focus) + 0x178,
                           &list, sizeof(list), nullptr) ||
        list.count < 0 || list.capacity < list.count || list.capacity > 100000 || (list.count && !list.data)) {
        Logf("npcs: FOCUSSCAN seq=%u cached_layout=invalid", serial);
        return false;
    }
    candidates.resize(static_cast<size_t>(list.count));
    if (list.count && !ReadProcessMemory(GetCurrentProcess(), list.data, candidates.data(),
                                         candidates.size() * sizeof(eCEntity*), nullptr)) {
        Logf("npcs: FOCUSSCAN seq=%u cached_read=failed", serial);
        candidates.clear();
        return false;
    }
    return true;
}

bool RelationOfImpl(eCEntity const* entity, RelationInfo& info)
{
    info = RelationInfo{};
    if (!entity) return false;
    uint8_t identity[16];
    Guid(const_cast<eCEntity*>(entity), identity);
    const GuidKey key = Key(identity);
    const auto mirror = mirrors.find(key);
    if (mirror != mirrors.end() && mirror->second.entity.GetInstance() == entity) {
        const Mirror& m = mirror->second;
        info.mirrored = true;
        info.bound = m.resolved && m.hostSession == net::WelcomeSerial();
        info.presenting = HostPresentationAvailable(entity);
        info.released = m.released;
        info.hostAttitude = m.latest.attitude;
        info.shown = m.attitudeShown;
        info.written = m.attitudeWritten;
        info.partyPlayer = m.latest.partyPlayer;
        info.aiMode = m.latest.aiMode;
        info.ageMs = m.latestTicks ? net::Now() - m.latestTicks : UINT32_MAX;
        info.drifts = m.attitudeDrifts;
    }
    const auto sent = lastSent.find(key);
    if (sent != lastSent.end()) {
        info.hasSent = true;
        info.sentAttitude = sent->second.record.attitude;
        info.sentAgeMs = net::Now() - sent->second.at;
    }
    return true;
}

void ResetWorldNpcTables()
{
    awaitedHits.clear();
    npclease::OnWorldDeactivate();
    playerContact = PlayerContact{};
    net::ResetNpcReplication();
    corpses.clear();
    absences.clear();
    absenceCheckAt = 0;
    namesSent.clear();
    claimedBy.clear();
    directedBy.clear();
    claimSeen.clear();
    myClaims.clear();
    givenBack.clear();
    contested.clear();
    hostTaken.clear();
    reclaimed.clear();
    reclaimPause.clear();
    sentStates.clear();
    inSphereSince.clear();
    deadOnHost.clear();
    lastSent.clear();
    for (auto& states : recipientNpcs) states.clear();
    std::fill(recipientGenerations, recipientGenerations + net::MaxPlayers, 0);
    incarnations.clear();
    hostHits.clear();
    forceHistory.clear();
    pendingForces.clear();
    forceSequences.clear();
    sendCadence = replication::Cadence60{};
    captureTicks = captureStartedAt = 0;
    owned.clear();
    sending.clear();
}

} // namespace detail

char const* MotionNameOf(uint32_t hash)
{
    auto name = motionNames.find(hash);
    return name == motionNames.end() ? nullptr : name->second.c_str();
}

eCEntity* NpcByGuid(uint8_t const guid[16])
{
    if (npcremoval::Suppressed(guid)) return nullptr;
    eCEntity* instance = Resolve(guid).GetInstance();
    return IsNpc(instance) ? instance : nullptr;
}

} // namespace g3t::npcs
