// SPDX-License-Identifier: GPL-3.0-or-later
// The host simulates NPCs around every player; clients replay buffered 60 Hz state without NPC AI.
#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>

class eCEntity;

namespace g3t::npcs {
void Start();
void Tick();
void OnWorldDeactivate();
bool IsMirrored(eCEntity const* entity);  // a client NPC currently driven from host snapshots
bool Ready(eCEntity const* entity);
bool PresentationPending(eCEntity const* entity);  // live, non-removed NPCs only; dead bodies do not need a new pose stream
void LocalHit(eCEntity* entity);  // our hero hit this mirror: its stagger shows here at once, as this game judged it (owner 2 Nov.: favour the client's feel)
void LocalHit(eCEntity* entity, uint32_t sequence);
void HostHit(eCEntity* entity, uint8_t player, uint32_t sequence);
void Despawn(eCEntity* entity);  // host: publish removal of this NPC incarnation, without client death/respawn scripts
uint32_t Incarnation(uint8_t const guid[16]);
void SetContext(eCEntity* entity, uint8_t player);
uint8_t ContextPlayer(eCEntity* entity);
bool Follow(uint8_t const guid[16], uint8_t player, uint8_t memberType, bool waiting = false);
bool Guide(uint8_t const guid[16], uint8_t player);
bool LocalReaction(eCEntity const* entity, char const* state);  // ai.cpp: the state is our own blow's stagger (or its helper) on this mirror: let it run
// The NPC with this GUID from the network: null unless it is an NPC (NPC, script routine and damage receiver property sets)
// and neither the local hero nor a puppet (Fable's review 22.10. 1.6, 1.7: handlers ran NPC wrappers on any entity).
eCEntity* NpcByGuid(uint8_t const guid[16]);
char const* MotionNameOf(uint32_t hash);  // a motion name another machine sent (net::SendMotionName), nullptr if unknown
bool InAction(eCEntity const* entity);   // a mirror plays an action motion (its idle task must not replace it)
bool DiedOnHost(eCEntity const* entity);  // a mirrored NPC the host reported dead
using LogFn = void (*)(char const*);
void BadRecord(LogFn log);  // QA, client (stage X1): an NPC record with the hero's GUID and a NaN position
// Relationship intents for recruitment and dismissal; NPC simulation always remains on the host.
void Claim(eCEntity* entity);
void Unclaim(eCEntity* entity);
bool OnClaim(uint8_t const guid[16], uint8_t player, bool take);  // host; false: refused (someone else has it)
// Compatibility no-op: a client cannot relocate an NPC when dismissing it.
void PlaceGivenBack(uint8_t const guid[16], float const position[3], float const rotation[4]);
void StoryMoved(uint8_t const guid[16], float const position[3], float const rotation[4]);  // a story move on the host (infocmd.cpp)
void Revoked(uint8_t const guid[16]);  // client: the host refused or ended our claim
void Rebind(uint8_t const guid[16]);   // look this mirror's GUID up again (its copy was just given that GUID: summon.cpp)
void Foreign(uint8_t const guid[16]);  // an NPC here that another machine simulates: never send it as ours (a copy)
void Adopt(uint8_t const guid[16]);    // client: a companion from the host's save is ours again
// Host: companions by player name, kept in the save's sidecar (lines "companion <guid hex> <player name>").
unsigned WriteCompanions(FILE* file);
unsigned ReadCompanions(FILE* file);
void ClearCompanions();
void CompanionsOff(LogFn log);  // QA negative control: saved companions are not handed over
void StyleOff(LogFn log);  // QA negative control: mirrors hold no weapons and keep their stance
void MirrorRetryOff(LogFn log);  // QA negative control (stage W8): a mirror missing at its first lookup stays missing (as before 23.10.)
void MirrorStats(LogFn log);     // QA (stage W8): MIRRORSTATS line (mirrors, missing ones, found later, expired)
void SyncReport(LogFn log);     // compact ownership and stale-mirror diagnostics
void WalkingReport(char const* guid, LogFn log);
void RecvOn(LogFn log);   // QA: take NPC snapshots again (stageM1: the host's win over this machine's own simulation)
void RecvOff(LogFn log);  // QA: ignore every NPC snapshot from now on (invariant self-test)
void DeadPoseOff(LogFn log);     // QA negative control: dead mirrors are not laid down
void DeadPoseReport(LogFn log);  // QA: DEADPOSE line
void LayDownCorpse(eCEntity* entity);  // a death applied here outside mirroring (journal.cpp): lay the body down like a dead mirror
unsigned HoldChanges();  // QA: weapons put into mirrors' hands so far
bool Claimed(eCEntity const* entity);  // client: taken over by this player
bool DeadStays();  // an NPC dead on the host stays dead everywhere (playtest 27.10.)
// It stood up again (respawn.cpp): no longer held dead here (code review 28.10. P2: the mark stayed, and once this client
// simulated the animal, KeepDead put it back to dead).
void NewLife(uint8_t const guid[16]);
// The co-op party leader, from an explicit client claim or the NPC's authoritative snapshot; 0 none.
uint8_t CompanionOwner(eCEntity* entity);
uint8_t ClaimedBy(eCEntity const* entity);  // host: the client that took this NPC over as its companion, 0 if none
void Mirrored(std::vector<eCEntity*>& out);  // client: the NPCs driven from another machine's snapshots now (guide.cpp)
// Host: an NPC a client simulates guides or follows the host's hero (guide.cpp): simulated here from now on, the client's
// snapshots of it refused (the client's copy becomes a mirror of the host's), until HostRelease. False if not mirrored.
bool HostTake(eCEntity* entity);
void HostRelease(eCEntity* entity);
bool HostTaken(eCEntity const* entity);
void SendInfo(char const* name, LogFn log);  // QA: NPCSEND line, why this machine last did not send the NPC (name or GUID), or sent
void LifeReport(char const* guid, LogFn log);  // QA: canonical/displayed incarnation, removal fence and replay readiness
void CorpseSleep(char const* guid, LogFn log);  // QA: disable a settled mirror's physics without a new death
void JoinGraceOff(LogFn log);  // QA negative control (host): take a joining client's snapshots of NPCs near the host
void ContestOff(LogFn log);    // QA negative control (host): take a client's NPC that this machine still processes just outside its range
void ClaimRecord(char const* guid, LogFn log);  // QA (client): one snapshot of that host NPC as this machine's, at its hero
void EdgeReport(LogFn log);    // QA (host): EDGENPC lines, the NPCs processed here 40-60 m from the nearest player
void HostWinsOff(LogFn log);  // QA negative control (client): keep simulating an NPC itself although the host sends it
void OwnOff(LogFn log);      // QA negative control (client): send none of its own NPCs
uint8_t RemoteOwner(uint8_t const guid[16]);  // remote authority, including a mirror not yet resolved/pacified; 0 if local
uint8_t OwnerOf(eCEntity const* entity);
// The owner's presentation time this mirror actually shows (0: no presentation yet), and on the owner
// whether that NPC parried (IsInParadeMode) in the state it sent at about that time (last 1.5 s; false: not known, or
// lagblockoff).
uint32_t SeenAt(eCEntity const* entity);
// QA (Stufe 4.1): frames of mirrors within 1 s after a replayed combat move, and how many of them had a weapon in the right
// hand whose damage type was set (the swing's hit window).
void SwingSamples(unsigned& armed, unsigned& samples);
// Stufe 4.2: a mirror within 30 m of our hero fights it (or its party), as its owner says: the fight music the game plays
// only for NPCs this machine simulates in combat mode.
bool MirrorFightsHero();
// QA: NPC snapshots this machine sent with the fighting bit; MirrorFightsHero calls that saw a fighting mirror at all, one
// fighting our hero, and one doing so within 30 m.
void FightStats(unsigned& sent, unsigned& seenAny, unsigned& seenUs, unsigned& seenNear);
bool ParryingAt(eCEntity const* entity, uint32_t at, bool& parrying);
void XpGuardOff(LogFn log);  // QA negative control: mirrors die through the game's death script again (and pay)
// Client: does an NPC's death that names no killer pay this machine's hero? Only for an NPC simulated here (sent in the
// last update, or claimed). The deaths a script causes far away (the orcs a freed town's script kills: Ardea_Orkboss,
// +150 on both machines, 25.10.) are the host's, which pays its own hero.
bool PaysDeathWithoutKiller(eCEntity const* victim);
void FarXpOff(LogFn log);  // QA negative control: such deaths pay the client's hero again
void Report(LogFn log);  // QA: one NPC line per NPC within 40 m of the local hero
bool Switch(char const* action, LogFn log);  // QA negative controls of the play-evening fixes (27.10.): true if it was one
}
