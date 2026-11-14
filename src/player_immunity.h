// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "downed_policy.h"
#include "net.h"
class eCEntity;
class Entity;
namespace g3t::downed {
extern PeerRecovery puppetRecovery[net::MaxPlayers];
extern InteractionProtection interaction;
extern eCEntity* interactionHero;
extern bool interactionWorldActive;
extern uint8_t lastInteractionFlags;
uint8_t InteractionOf(eCEntity*);
bool TrackPuppetRecovery(int);
void ResetPeerImmunity(unsigned player);
void OfferHelp(Entity&, net::Remote const*, uint32_t now);
void Wide(char const*, wchar_t*, int);
void WatchBlows(Entity&, uint32_t);
void StandUp(Entity&);
}
