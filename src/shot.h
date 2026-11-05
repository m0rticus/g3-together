// SPDX-License-Identifier: GPL-3.0-or-later
// Shots (P2 of DESIGN-2026-10-19; arrows first). An arrow exists only where it was shot: the game's Shoot
// (PS_Ranged.cpp; re/findings/ranged-ai-paths-b.md) spawns it on the shooter's machine with the shooter as its
// Interaction owner. Here a player's own arrow is announced as it leaves the bow (template, pose, direction, speed,
// path); the others fly a copy that belongs to that player's puppet, so the combat filter drops its hits (rule 1): it
// shows everywhere. NPC projectile copies judge contacts with the recipient's own hero only; shared NPC
// damage remains on the simulation authority. Native player-shot behavior retains its separate path.
#pragma once

class eCEntity;

namespace g3t::shot {
void Start();
void OnWorldChange();
bool NpcReplica(eCEntity const* entity);
bool InNpcReplicaContact();
void Tick();  // once per frame, after QA (a QA shot is re-aimed there first): installs the hook, announces new arrows
using LogFn = void (*)(char const*);
void NpcCast(char const* argument, LogFn log);  // QA: GUID_spell-item, a real NPC projectile cast at the other player
void Report(LogFn log);       // QA: SHOTSTATE line (counters) and SHOTCOPY lines (the latest copies and their paths)
void CopyHeroOff(LogFn log);  // QA negative control: copies belong to our own hero, so their hits count again
void MirrorOff(LogFn log);    // QA negative control: shots are not announced
void TouchLog(LogFn log);     // QA: hook gCProjectile_PS::OnTouch (first call) and print the latest touches as TOUCH lines
}
