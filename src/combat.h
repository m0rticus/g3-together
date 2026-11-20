// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

class eCEntity;

namespace g3t::combat {
void Start();
void Tick();
void OnWorldChange();
bool IgnoreFriendlyHit(eCEntity* victim, eCEntity* source, unsigned player = 0);
using LogFn = void (*)(char const*);
void SyncReport(LogFn log);
bool InParadeMode(eCEntity* entity);
bool LocalBlow(eCEntity* attacker);
char const* LastLocalBlowRejectReason();
bool PlayerBlow(eCEntity* victim);
bool IsGroundImmuneNpc(eCEntity* entity);
bool LethalHit(eCEntity* victim);
bool ProjectileHit(eCEntity* victim, eCEntity* projectile);
void ResolveKillXp(eCEntity* victim, eCEntity* killer, bool wasDefeated = false);
}
