// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-free policy deciding whether a client-side NPC entity may be visible, pickable, and collidable.
// Clients load G3T_client_template with every world NPC at its authored initial spawn; an entity on the client
// may be presented only while backed by active host presentation, a settled host corpse, or a host-vetted client lease.
#pragma once

#include <cmath>
#include <cstdint>

namespace g3t::npcpresence {

// Native Script_Game Respawn sets gCNPC_PS::Species = 40 and teleports to (-220000, 100000, 100000) on permanent despawn.
constexpr int DespawnedSpecies = 40;
constexpr float VoidDespawnX = -220000.0f;
constexpr float VoidDespawnY = 100000.0f;
constexpr float VoidDespawnZ = 100000.0f;
constexpr float VoidDespawnToleranceCm = 1024.0f;

inline bool IsVoidDespawnPosition(float const position[3]) noexcept
{
    if (!position) return false;
    return std::fabs(position[0] - VoidDespawnX) <= VoidDespawnToleranceCm &&
           std::fabs(position[1] - VoidDespawnY) <= VoidDespawnToleranceCm &&
           std::fabs(position[2] - VoidDespawnZ) <= VoidDespawnToleranceCm;
}

inline float PositionDriftSq(float const a[3], float const b[3]) noexcept
{
    const float dx = a[0] - b[0];
    const float dy = a[1] - b[1];
    const float dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

inline bool OfferMatchesLocalBaseline(float const localPos[3], int32_t localHp, uint8_t localDead,
                                      float const hostPos[3], int32_t hostHp, uint8_t hostDead) noexcept
{
    return localDead == hostDead && localHp == hostHp &&
           PositionDriftSq(localPos, hostPos) <= 10000.0f;
}

enum class Verdict : uint8_t {
    Present,
    Hidden,
};

struct Facts {
    bool isHost = false;
    bool character = false;
    bool playerOrPuppet = false;
    bool removed = false;
    bool duplicateQuarantined = false;
    bool despawned = false;
    bool clientDialogPartner = false;
    bool clientGuideActive = false;
    bool clientPreviouslyVetted = false;
    bool hostMirrorBound = false;
    bool hostMirrorPlaced = false;
    bool hostMirrorDead = false;
    bool hostMirrorReleased = false;
    bool hostReleasePending = false;
    bool hostCorpseBound = false;
    bool clientLeaseVetted = false;
    bool clientHandbackPending = false;
};

inline Verdict Decide(Facts const& f) noexcept
{
    if (!f.character) return Verdict::Present;
    if (f.removed || f.duplicateQuarantined) return Verdict::Hidden;
    if (f.isHost || f.playerOrPuppet) return Verdict::Present;
    if (f.despawned) return Verdict::Hidden;
    if (f.clientDialogPartner) return Verdict::Present;
    if (f.hostMirrorBound && f.hostMirrorPlaced &&
        (f.hostMirrorDead || !f.hostMirrorReleased || f.hostReleasePending || f.clientGuideActive)) {
        return Verdict::Present;
    }
    if (f.hostCorpseBound) return Verdict::Present;
    if (f.clientGuideActive && f.clientPreviouslyVetted) return Verdict::Present;
    if (f.clientLeaseVetted || f.clientHandbackPending) return Verdict::Present;
    return Verdict::Hidden;
}

} // namespace g3t::npcpresence
