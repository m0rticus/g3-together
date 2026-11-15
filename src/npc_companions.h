// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npc_state.h"

namespace g3t::npcs::detail {

bool AdmitIncomingNpc(net::NpcRecord const& record, GuidKey const& key, uint32_t captured,
                      uint8_t owner, uint32_t now);
void ResetMirrorForNewStream(Mirror& m, GuidKey const& key, net::NpcRecord const& record, bool sameOwner);
void PackMotionFromRecord(body::MotionState& motion, net::NpcRecord const& r);
void CopyMotionToRecord(net::NpcRecord& shown, body::Presentation const& presentation);
void SyncMirrorPartyAndGuide(GuidKey const& key, Mirror& m, net::NpcRecord const& shown);
bool YieldMirrorForClientTalk(Mirror& m, uint8_t flags, uint32_t now);
void DropMirror(GuidKey const& key);
void SendRevoke(GuidKey const& key, uint8_t player);
void RefreshCompanionIdentity();
void OfferCompanions(uint32_t now);
void ClaimTick(uint32_t now);

} // namespace g3t::npcs::detail
