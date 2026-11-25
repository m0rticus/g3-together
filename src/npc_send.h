// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npc_state.h"
#include "npcsend_candidates.h"

#include <unordered_set>

namespace g3t::npcs::detail {

struct HostSendCandidateEntity {
    eCEntity* instance = nullptr;
    bool ambiguous = false;
    bool fromProcessed = false;
};

struct HostSendFrame {
    std::unordered_map<GuidKey, HostSendCandidateEntity, GuidHash> entities;
    eCEntity* partyLeaderOf[net::MaxPlayers] = {};
    bool recipientHere[net::MaxPlayers] = {};
    uint32_t stretchOf[net::MaxPlayers] = {};

    void Clear()
    {
        entities.clear();
        std::fill(std::begin(partyLeaderOf), std::end(partyLeaderOf), nullptr);
        std::fill(std::begin(recipientHere), std::end(recipientHere), false);
        std::fill(std::begin(stretchOf), std::end(stretchOf), 0u);
    }

    void NoteCandidate(GuidKey const& key, eCEntity* instance, bool fromProcessed)
    {
        if (!instance || IsZeroGuid(key)) return;
        auto [it, inserted] = entities.try_emplace(key);
        if (inserted) {
            it->second.instance = instance;
        } else if (it->second.instance != instance) {
            it->second.ambiguous = true;
        }
        if (fromProcessed) it->second.fromProcessed = true;
    }
};

int OnRelease(uint8_t owner, uint32_t generation, uint8_t* guids, int count);
void OnMotionName(uint32_t hash, char const* name);
uint8_t FightFlags(Entity& npc);
void PrepareHostSendFrame(std::vector<npcsend_candidates::GuidCandidate> const& candidates,
                          size_t firstProcessed, unsigned candidateKeyCount, unsigned processedCount,
                          HostSendFrame& out);
eCEntity* AdmitHostSendCandidate(GuidKey const& candidateKey, eCEntity* hero, eCEntity* controlled,
                                 HostSendFrame const& frame, float const players[][3], int playerCount,
                                 uint8_t outGuid[16], GuidKey& outKey, float outAt[3]);
bool CheckHostSendRange(eCEntity* instance, GuidKey const& key, float const at[3],
                        float const players[][3], int playerCount, SendNote& note);
void CaptureAndRecordOwnedNpc(Entity& self, eCEntity* instance, GuidKey const& key,
                              eCEntity* const partyLeaderOf[net::MaxPlayers], net::NpcRecord& r,
                              npc_equipment::BackEquipment& backEquip, std::vector<pose::Bone>& bones,
                              uint8_t& poseFlags, bool& posed, bool& moving);
void SendNpcToRecipients(Entity& self, eCEntity* instance, eCEntity* hostFocus, eCEntity* hostTarget,
                         GuidKey const& key, uint8_t const guid[16], float const at[3], net::NpcRecord const& r,
                         npc_equipment::BackEquipment const& backEquip, std::vector<pose::Bone> const& bones,
                         uint8_t poseFlags, bool posed, bool moving, uint32_t const stretchOf[net::MaxPlayers]);
void FinishOwnedTick(eCEntity* hero, float const players[][3], int playerCount);
void HostTick(void (*sendOwned)(eCEntity*, float const[][3], int));

} // namespace g3t::npcs::detail
