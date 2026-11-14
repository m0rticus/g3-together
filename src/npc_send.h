// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npc_state.h"

namespace g3t::npcs::detail {

int OnRelease(uint8_t owner, uint32_t generation, uint8_t* guids, int count);
void OnMotionName(uint32_t hash, char const* name);
uint8_t FightFlags(Entity& npc);
bool CheckHostSendRange(eCEntity* instance, GuidKey const& key, float const at[3],
                        float const players[][3], int playerCount, SendNote& note);
void CaptureAndRecordOwnedNpc(Entity& self, eCEntity* instance, GuidKey const& key, net::NpcRecord& r,
                              std::vector<pose::Bone>& bones, uint8_t& poseFlags, bool& posed, bool& moving);
void SendNpcToRecipients(Entity& self, eCEntity* instance, eCEntity* hostFocus, eCEntity* hostTarget,
                         GuidKey const& key, uint8_t const guid[16], float const at[3], net::NpcRecord const& r,
                         std::vector<pose::Bone> const& bones, uint8_t poseFlags, bool posed, bool moving,
                         uint32_t const stretchOf[net::MaxPlayers]);
void FinishOwnedTick(eCEntity* hero, float const players[][3], int playerCount);
void HostTick(void (*sendOwned)(eCEntity*, float const[][3], int));

} // namespace g3t::npcs::detail
