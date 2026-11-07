// SPDX-License-Identifier: GPL-3.0-or-later
#include "downed.h"
#include "downed_policy.h"

#include "body.h"
#include "crashlog.h"
#include "hud.h"
#include "i18n.h"
#include "items.h"
#include "log.h"
#include "net.h"
#include "persist.h"
#include "remote.h"

#include <Game.h>
#include <Script.h>
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace g3t::downed {
namespace {
bool enabled = true;      // death becomes a knock-out
bool holdEnabled = true;  // a knocked-out hero waits for a partner
bool potionNeeded = true; // QA negative control: potionoff (helping up is free again)
// Only a death the mod turned into a knock-out (RedirectDeath) waits for help (PLAN-2026-10-22 2.2, re/findings/
// defeat-arena-c.md). A knock-out of the game's own (guards, a duel, the arena: Defeat) takes the game's course:
// the winner may rob the hero, the hero stands up by itself. Before, every knock-out was held (spieltest 21.10.
// 21:35: guards knocked a player out, a partner paid a potion, nobody robbed him).
bool lethalOnly = true;   // QA negative control: lethalonlyoff (every knock-out waits for help again)
bool lethalBlows = true;  // QA negative control: lethalblowsoff (a knock-out of the game's own is never a death, as before 3 Nov.)
unsigned redirects = 0, revives = 0, releases = 0, deaths = 0, potionsPaid = 0, refusedNoPotion = 0;
// Helping up through a dialog (owner 23.10.): a partner waiting for help gets an option on its puppet, the player's line
// "Ich werde dir helfen." (INFO_SKALVERAM320060309153144, C trade-screen-c.md 6.); chosen, the hero pays a healing potion and
// helps it up. Standing next to it for 3 s no longer does (QA autohelpon: that again).
bool autoHelp = false;
bool helpOption = true;  // QA negative control: helpoptionoff (no option is offered)
char const* const HelpLine = "INFO_SKALVERAM320060309153144";
// The help counts only once its line is spoken (infocmd.cpp sees the Say): the info manager's current info was ours as
// soon as a dialog with the puppet began, so talking to a downed partner, or choosing the trade, helped it up and paid a
// potion without the line being chosen (the host's report, playtest 27.10.: "chose to help" in the same ms as the trade).
bool helpLineOnly = true;  // false: as before 27.10.
uint32_t helpSaidAt = 0;
unsigned helpNotSaid = 0;
bool helpStarted[net::MaxPlayers] = {};  // our option for player i is the running info (once per choice)
uint32_t lastOffer = 0;
unsigned optionsOffered = 0, optionsChosen = 0;
char const* const KnockOutTask = "ZS_LieKnockDown";
char const* const PotionPrefix = "It_Potion_Health";  // healing potions (the game's own templates)
constexpr float ReviveRange = 250.0f;  // cm: a partner standing next to the hero
constexpr uint32_t ReviveTime = 3000;  // ms the partner has to stay there
constexpr uint32_t StaleState = 5000;  // ms: a remote state older than this does not count
constexpr uint32_t PaidCooldown = 5000; // ms: the revived player's state still says "down" for a moment

Life life;                     // authority is the paid revive, not the native body's current state
bool lethal = false;            // the knock-out it lies in replaced its death (RedirectDeath)
bool lethalSeenDown = false;    // ... and the hero has lain down since (until then a frame without IsDown is no standing up)
uint32_t lethalAt = 0;          // when RedirectDeath set it
int lastHp = -1;                // the hero's hit points a frame ago
uint32_t lethalBlowAt = 0;      // when a blow by someone who means to kill took hit points off the hero
unsigned nativeLethal = 0;      // knock-outs of the game's own that were a lethal blow (promoted to the partner's help)
bool nativeNoted = false;       // logged once per knock-out of the game's own
bool terminalKillSent = false;  // the native Kill/game-over script was called once for this life
std::string inHand[2];         // the hero's right and left hand while standing (a knock-out drops them)
bool wasDown = false;
uint32_t aloneSince = 0;        // when the knocked-out hero last had nobody known able to help (0: someone can)
int revivedBy = 0;              // a partner paid for our revive (set by the network, handled in Tick)
bool told[net::MaxPlayers] = {};  // this player already saw "X is down" for player i
bool toldDead[net::MaxPlayers] = {};
uint32_t helpingSince[net::MaxPlayers] = {};  // local hero next to downed player i since then (0: not there)
bool hinted[net::MaxPlayers] = {};            // "you need a potion" shown for this stay next to player i
uint32_t paidAt[net::MaxPlayers] = {};        // when we paid a potion for player i
uint32_t paidSequence[net::MaxPlayers] = {};
uint32_t pendingReviveSequence = 0;
unsigned transitions = 0, corrections = 0;
uint32_t lastCorrection = 0;

void Transition(char const* reason, char const* source)
{
    Logf("downed: transition=%u player=%u role=%s held=%d terminal=%d sequence=%u reason=%s source=%s",
         ++transitions, net::LocalId(), net::IsHost() ? "host" : "client", life.held ? 1 : 0,
         life.terminal ? 1 : 0, life.sequence, reason, source);
}

void EndStep(char const* step)
{
    crashlog::EndStep(step);
    Logf("downed: end step=%s player=%u role=%s held=%d terminal=%d kill_sent=%d sequence=%u",
         step, net::LocalId(), net::IsHost() ? "host" : "client", life.held ? 1 : 0,
         life.terminal ? 1 : 0, terminalKillSent ? 1 : 0, life.sequence);
}

bool DownTask(char const* task)
{
    return !std::strcmp(task, KnockOutTask) || !std::strcmp(task, "ZS_Unconscious") || !std::strcmp(task, "ZS_PiercedKO");
}

bool DownPose(Entity& hero)
{
    const auto pose = static_cast<gEAniState>(hero.Routine.AniState);
    return pose == gEAniState_LieKnockDown || pose == gEAniState_LieKnockOut || pose == gEAniState_LiePiercedKO;
}

void KeepDown(Entity& hero, char const* source)
{
    const bCString task = hero.Routine.GetCurrentTask(), state = hero.Routine.GetCurrentState();
    const bool repair = !hero.IsDown() || !DownTask(task.GetText()) ||
                        (std::strcmp(state.GetText(), "ZS_Unconscious_Loop") && !DownTask(state.GetText())) ||
                        (!std::strcmp(state.GetText(), "ZS_Unconscious_Loop") && !DownPose(hero));
    const int hp = hero.PlayerMemory.GetHitPoints();
    if (repair || hp > 1) {
        ++corrections;
        if (repair || !lastCorrection || net::Now() - lastCorrection >= 5000) {
            lastCorrection = net::Now();
            Logf("downed: hold correction=%u player=%u sequence=%u source=%s task=%s state=%s ai=%d ani=%d hp=%d timer=%.3f repair=%d",
                 corrections, net::LocalId(), life.sequence, source, task.GetText(), state.GetText(),
                 static_cast<int>(hero.Routine.AIMode), static_cast<int>(hero.Routine.AniState), hp,
                 hero.Routine.GetTaskTime(), repair ? 1 : 0);
        }
    }
    if (repair) {
        hero.Routine.FullStop();
        hero.CharacterMovement.StopMovement();
        hero.Routine.SetTask(bCString(KnockOutTask));
    }
    // OnPlayerGameKeyPressed refuses AI mode Down (Script_Game.dll 0x10061640).
    hero.Routine.AIMode = gEAIMode_Down;
    hero.Routine.SetTaskTime(0.0f);
    if (hp != 1) hero.PlayerMemory.SetHitPoints(1);
}

void CancelAlone(char const* source);

void Hold(char const* reason, char const* source)
{
    if (life.held || life.terminal) return;
    life.Restore(true);
    CancelAlone("new-held-life");
    revivedBy = 0;
    pendingReviveSequence = 0;
    Transition(reason, source);
    hud::Message(hud::Bad, potionNeeded ? i18n::Text(i18n::KnockedOutNeedsPotion) : i18n::Text(i18n::KnockedOut));
}

Availability HelperState(net::Remote const& r, uint32_t now)
{
    return HelperAvailability(r.connected, r.activity.known, r.activity.ticks, r.activity.flags, now);
}

// Being able to travel back and help is independent of the loaded/presented body and world location.
bool Able(net::Remote const& r, uint32_t now)
{
    return HelperState(r, now) == Availability::Able;
}

// Paying/interacting still needs a current nearby body in this ready gameplay world.
bool Present(net::Remote const& r, uint32_t now)
{
    return r.connected && r.latestTicks && now - r.latestTicks < StaleState && (r.latest.flags & 1) &&
           r.latest.world == remote::CurrentWorld() && net::InWorld(r.latest.id);
}

// Every connected partner is playing and down, knocked out, or dead, wherever its body is.
bool AllOthersDown(net::Remote const* remotes, uint32_t now)
{
    bool present = false;
    for (int i = 1; i < net::MaxPlayers; ++i) {
        if (i == net::LocalId()) continue;
        net::Remote const& r = remotes[i];
        if (!r.connected) continue;
        if (!Playing(HelperState(r, now))) return false;
        present = true;
        if (HelperState(r, now) != Availability::Down) return false;
    }
    return present;
}

char const* HelperReason(Availability state)
{
    switch (state) {
    case Availability::Able: return "able";
    case Availability::Down: return "down-or-dead";
    case Availability::Disconnected: return "disconnected";
    case Availability::Unknown: return "no-activity";
    case Availability::Stale: return "activity-stale";
    case Availability::NotPlaying: return "loading-or-menu";
    }
    return "invalid";
}

void LogHelpers(Entity* hero, char const* event, uint32_t now)
{
    float here[3] = {};
    if (hero) {
        const bCVector& at = hero->GetInstance()->GetWorldPosition();
        here[0] = at.GetX(); here[1] = at.GetY(); here[2] = at.GetZ();
    }
    for (int i = 1; i < net::MaxPlayers; ++i) {
        if (i == net::LocalId()) continue;
        net::Remote const& r = net::Remotes()[i];
        Logf("downed: helpers event=%s player=%u partner=%d name=%s connected=%d joined=%d gate=%d "
             "activity_known=%d activity_age=%u activity_packet=%u flags=%u loading=%d menu=%d "
             "pose_known=%d pose_age=%u pose_packet=%u pose_flags=%u sequence=%u "
             "world=%08x partner_world=%08x pose_world=%08x epoch=%08x partner_epoch=%08x "
             "distance_cm=%.0f local_position_known=%d zone=not-sent reject=%s",
             event, net::LocalId(), i, *r.latest.name ? r.latest.name : "-", r.connected ? 1 : 0,
             (net::IsHost() ? net::PeerJoined(static_cast<uint8_t>(i)) : net::Joined() && r.connected) ? 1 : 0,
             net::InWorld(static_cast<uint8_t>(i)) ? 1 : 0, r.activity.known ? 1 : 0,
             r.activity.known ? now - r.activity.ticks : 0, r.activity.sequence, r.activity.flags,
             (r.activity.flags & 32) ? 1 : 0, (r.activity.flags & 64) ? 1 : 0,
             r.latestTicks ? 1 : 0, r.latestTicks ? now - r.latestTicks : 0, r.latest.sequence,
             r.latest.flags, r.activity.downedSequence, hero ? remote::CurrentWorld() : 0, r.activity.world,
             r.latest.world, net::WorldEpoch(), r.activity.epoch,
             hero && r.latestTicks ? body::Distance(r.latest.position, here) : -1.0f,
             hero ? 1 : 0, HelperReason(HelperState(r, now)));
    }
}

void CancelAlone(char const* source)
{
    if (!aloneSince) return;
    const uint32_t now = net::Now();
    // World retirement/restore must not inspect the old hero to report cancellation.
    LogHelpers(nullptr, "alone-cancel", now);
    Logf("downed: alone reset player=%u source=%s elapsed_ms=%u", net::LocalId(), source, now - aloneSince);
    aloneSince = 0;
}

void Wide(char const* text, wchar_t* out, int size)
{
    out[0] = 0;
    if (!MultiByteToWideChar(CP_UTF8, 0, text, -1, out, size)) out[0] = 0;
    out[size - 1] = 0;
}

void StandUp(Entity& hero)
{
    const int max = hero.PlayerMemory.GetHitPointsMax();
    hero.Routine.FullStop();
    hero.PlayerMemory.SetHitPoints(max / 4 > 1 ? max / 4 : 1);
    hero.Routine.ContinueRoutine();  // like the knock-out loop itself: Rtn_Player stands the hero up
}

// The weakest healing potion the hero carries (cheapest by gold value), or -1.
GEInt SmallestPotion(Entity& hero, char* name, size_t size)
{
    GEInt best = -1, bestValue = 0;
    for (GEInt i = 0; i < hero.Inventory.GetStackCount(); ++i) {
        Entity item = hero.Inventory.GetTemplateItem(i);
        if (!item.GetInstance() || hero.Inventory.GetAmount(i) < 1) continue;
        if (std::strncmp(item.GetName().GetText(), PotionPrefix, std::strlen(PotionPrefix)) != 0) continue;
        const GEInt value = item.Item.GetGoldValue();
        if (best < 0 || value < bestValue) { best = i; bestValue = value; strncpy_s(name, size, item.GetName().GetText(), _TRUNCATE); }
    }
    return best;
}

void SendToPlayer(uint8_t kind, int player, uint32_t sequence)
{
    net::WorldState s{};
    s.kind = kind;
    s.guard = static_cast<uint8_t>(player);      // to this player
    s.npcClass = net::LocalId();                 // from this one (the host passes it on)
    std::memcpy(s.guid, &sequence, sizeof(sequence)); // kind 8/9 has no entity GUID: bind it to the target's downed life
    net::SendWorldState(s, 0);
}

// The local hero helps player i up: it pays a healing potion (the smallest it carries) or is told it needs one.
void PayAndHelp(Entity& hero, int i, net::Remote const& r, uint32_t now)
{
    const bCVector& at = hero.GetInstance()->GetWorldPosition();
    const float here[3] = {at.GetX(), at.GetY(), at.GetZ()};
    if (life.held || life.terminal || hero.IsDown() || hero.IsDead() || i == net::LocalId() || !Present(r, now) ||
        !(r.latest.flags & 8) || (r.latest.flags & 4) || !r.latest.downedSequence ||
        body::Distance(r.latest.position, here) > ReviveRange ||
        (paidSequence[i] == r.latest.downedSequence && now - paidAt[i] < PaidCooldown)) {
        Logf("downed: help refused player=%u target=%d sequence=%u reason=not-eligible", net::LocalId(), i, r.latest.downedSequence);
        return;
    }
    wchar_t name[32], text[200];
    Wide(r.latest.name, name, 32);
    char potion[64] = "";
    const GEInt stack = potionNeeded ? SmallestPotion(hero, potion, sizeof(potion)) : -1;
    if (potionNeeded && stack < 0) {
        if (!hinted[i]) {
            hinted[i] = true;
            ++refusedNoPotion;
            swprintf_s(text, i18n::Text(i18n::NeedHealingPotion), name);
            hud::Message(hud::Warning, text);
            SendToPlayer(9, i, r.latest.downedSequence);
            Logf("downed: wants to help player %d without a healing potion", i);
        }
        return;
    }
    if (stack >= 0) {
        hero.Inventory.DeleteItems(stack, 1);
        ++potionsPaid;
    }
    paidAt[i] = now;
    paidSequence[i] = r.latest.downedSequence;
    helpingSince[i] = 0;
    hinted[i] = false;
    SendToPlayer(8, i, r.latest.downedSequence);
    swprintf_s(text, potionNeeded ? i18n::Text(i18n::HelpedUpWithPotion) : i18n::Text(i18n::HelpedUp), name);
    hud::Message(hud::Good, text);
    Logf("downed: helped player %d up, paid %s sequence=%u source=help-line", i, *potion ? potion : "nothing", r.latest.downedSequence);
}

// QA autohelpon (as before 23.10.): the local hero stands next to a downed partner: after ReviveTime it helps it up.
void Help(Entity& hero, net::Remote const* remotes, uint32_t now)
{
    const bCVector& at = hero.GetInstance()->GetWorldPosition();
    const float here[3] = {at.GetX(), at.GetY(), at.GetZ()};
    for (int i = 1; i < net::MaxPlayers; ++i) {
        if (i == net::LocalId()) continue;
        net::Remote const& r = remotes[i];
        const bool down = Present(r, now) && (r.latest.flags & 8) && !(r.latest.flags & 4);  // waiting for help, not any knock-out
        if (!down || body::Distance(r.latest.position, here) > ReviveRange || now - paidAt[i] < PaidCooldown) {
            helpingSince[i] = 0;
            hinted[i] = false;
            continue;
        }
        if (!helpingSince[i]) { helpingSince[i] = now; continue; }
        if (now - helpingSince[i] < ReviveTime) continue;
        PayAndHelp(hero, i, r, now);
    }
}

// The manager finds a speaker's infos through a map by owner it builds when it loads them; AddInfo does not enter a new
// one there, so it never showed in a dialog (stageA2 24.10.: "G3T_HelpUp_2" could not be chosen). The game's own
// BuildMapInfosByOwners (private, exported by Game.dll) builds the map again from all infos.
void RebuildOwnerMap(gCInfoManager_PS* manager)
{
    using Build = void(__thiscall*)(gCInfoManager_PS*);
    static Build build = reinterpret_cast<Build>(GetProcAddress(GetModuleHandleA("Game.dll"), "?BuildMapInfosByOwners@gCInfoManager_PS@@AAEXXZ"));
    if (build) build(manager);
    else Log("downed: BuildMapInfosByOwners not found; the help option will not show");
}

// Player i's puppet offers our help while i waits for it (its info is hidden otherwise); the option chosen, we pay and
// help. Infos are looked up by name, never kept: a load makes the info manager anew.
void OfferHelp(Entity& hero, net::Remote const* remotes, uint32_t now)
{
    gCInfoManager_PS* manager = gCInfoManager_PS::GetInstance();
    if (!manager) return;
    gCInfo_PS* running = manager->AccessIsRunning() ? manager->GetCurrentInfo() : nullptr;
    const bool scan = now - lastOffer >= 250;
    if (scan) lastOffer = now;
    for (int i = 1; i < net::MaxPlayers; ++i) {
        if (i == net::LocalId()) continue;
        char infoName[32];
        Format(infoName, "G3T_HelpUp_%d", i);
        // Chosen: the running info is ours (once per choice; the option hides itself when it ran).
        const bool ours = running && !std::strcmp(running->AccessName().GetText(), infoName);
        // Or the game marked the shown option given: it does when the option runs (shown, it is not given, OfferHelp below).
        gCInfo_PS* helpInfo = ours ? manager->GetInfo(bCString(infoName)) : nullptr;
        const bool said = !helpLineOnly || (helpSaidAt && now - helpSaidAt < 5000) || (helpInfo && helpInfo->AccessInfoGiven());
        if (ours && !helpStarted[i] && !said) ++helpNotSaid;
        if (ours && !helpStarted[i] && said) {
            helpSaidAt = 0;
            helpStarted[i] = true;
            ++optionsChosen;
            Logf("downed: the player chose to help player %d up", i);
            hinted[i] = false;  // every choice without a potion says so again
            PayAndHelp(hero, i, remotes[i], now);
        }
        if (!ours) helpStarted[i] = false;
        if (!scan) continue;
        net::Remote const& r = remotes[i];
        const bool waiting = helpOption && Present(r, now) && (r.latest.flags & 8) && !(r.latest.flags & 4) && now - paidAt[i] >= PaidCooldown;
        gCInfo_PS* info = manager->GetInfo(bCString(infoName));
        if (!info && waiting) {
            info = manager->AddInfo(bCString(infoName));
            if (!info) continue;
            char owner[32];
            Format(owner, "G3T_Player%d", i);  // the puppet's name (remote.cpp)
            info->AccessOwner() = bCString(owner);
            info->AccessConditionType() = gEInfoCondType_General;
            info->AccessType() = gEInfoType_Info;
            info->AccessPermanent() = GEFalse;
            info->AccessInfoGiven() = GETrue;
            if (gCInfoScript_PS* script = info->AccessInfoScript().GetObject())
                script->AddCommand(bCString("Say"), bCString("player"), bCString(""), bCString(""), bCString(""), eCLocString(bCString(HelpLine)));
            RebuildOwnerMap(manager);
            Logf("downed: help option for player %d made (%s)", i, owner);
        }
        if (!info) continue;
        // Shown (not given) while i waits, hidden (given) otherwise; running it marks it given, so a choice without a
        // potion shows it again on the next look while i still waits.
        const bool shown = !info->AccessInfoGiven();
        if (waiting && !shown && !ours) {
            info->AccessInfoGiven() = GEFalse;
            ++optionsOffered;
        } else if (!waiting && shown) {
            info->AccessInfoGiven() = GETrue;
        }
    }
}

// The game knocks a human out where it would kill an animal (its hit judgement: a hero hit by an orc, a goblin with a club or
// a man with a weapon lies down at 0 hit points, then stands up by itself), so the hero never died in a fight with them and
// the help-up rule above never applied: 13 knock-outs and no death in the 2 Nov. play session (owner: "we can't die now").
// Each blow that takes hit points off the hero is noted when its attacker means to kill (the game's HasAttackReasonToKill:
// raiders, monsters, an intruder; not a duel, the arena or a fight with another player), and a knock-out at 0 hit points that
// follows such a blow is held for a partner like a death that the mod turned into a knock-out.
void WatchBlows(Entity& hero, uint32_t now)
{
    const int hp = hero.PlayerMemory.GetHitPoints();
    const int before = lastHp;
    lastHp = hp;
    if (before < 0 || hp >= before || !net::Connected()) return;
    eCEntity* attacker = hero.NPC.GetCurrentAttacker().GetInstance();
    if (!attacker || remote::IsPuppet(attacker) || !attacker->GetPropertySet(eEPropertySetType_NPC)) return;
    if (GetScriptAdminExt().CallScript(bCString("HasAttackReasonToKill"), attacker, hero.GetInstance(), 0)) lethalBlowAt = now ? now : 1;
}

// A partner is in the game (any world): only then does a lethal knock-out wait for help, else the game's own rule stands as in
// single-player.
bool PartnerPresent(net::Remote const* remotes, uint32_t now)
{
    for (int i = 1; i < net::MaxPlayers; ++i)
        if (i != net::LocalId() && Playing(HelperState(remotes[i], now))) return true;
    return false;
}

// Known all-down state is immediate; unknown partner state waits for AloneGrace. Both paths end this life once.
// The native Kill script shows game over immediately. Starting ZS_Dead as well would call Kill a second time.
void Die(Entity& hero, char const* reason)
{
    if (!life.End()) {
        EndStep("skipped-terminal");
        return;
    }
    lethal = false;
    lethalSeenDown = false;
    revivedBy = 0;
    pendingReviveSequence = 0;
    EndStep("decision");
    Transition(reason, "end-policy");
    ++deaths;
    Logf("downed: nobody can help; the hero dies reason=%s hero=%p hp=%d task=%s state=%s ai=%d",
         reason, hero.GetInstance(), hero.PlayerMemory.GetHitPoints(), hero.Routine.GetCurrentTask().GetText(),
         hero.Routine.GetCurrentState().GetText(), static_cast<int>(hero.Routine.AIMode));
    const uint32_t now = net::Now();
    LogHelpers(&hero, reason, now);
    hud::Message(hud::Bad, i18n::Text(i18n::NobodyCanHelp));
    EndStep("set-hp");
    hero.PlayerMemory.SetHitPoints(0);
    EndStep("full-stop");
    hero.Routine.FullStop();
    // Kill(killer, victim), unlike ResetAll(self): Script_Game.dll RVA 0x1bc30 resets OTHER before showing game over.
    // No killer is needed here; a null victim crashes in ResetAll -> Entity::SetLookAtTarget.
    terminalKillSent = true;
    EndStep("kill-enter");
    const GEInt killed = GetScriptAdminExt().CallScript(bCString("Kill"), nullptr, hero.GetInstance(), 0);
    EndStep("kill-return");
    Logf("downed: native Kill/game-over script called once (result=%d)", killed);
}
}

// A hero's death runs through the tasks ZS_Dead or ZS_RagDollDead (the hit judgement and the kill cheat set
// them); their script Kill then shows "Game over" and the player can only load a save (stage A exploration:
// the hero lay dead with the session still running). The knock-out task ends with the hero standing up again
// (from 0 hit points to 20 to 30 within 10 s in the same exploration), so it replaces the death, unless nobody
// can help any more (Die).
bool RedirectDeath(eCEntity* self, char const* state)
{
    if (!enabled || life.terminal || !self || self != gCSession::GetInstance().GetHero()) return false;
    if (std::strcmp(state, "ZS_Dead") != 0 && std::strcmp(state, "ZS_RagDollDead") != 0) return false;
    Entity hero(self);
    hero.Routine.FullStop();
    hero.Routine.SetTask(bCString(KnockOutTask));
    lethal = life.held || !persist::HeroPending();
    lethalSeenDown = false;
    lethalAt = net::Now();
    if (holdEnabled && !persist::HeroPending()) Hold("redirect-death", state);
    ++redirects;
    Logf("downed: hero would die (%s), knocked out instead (hp=%d)", state, hero.PlayerMemory.GetHitPoints());
    return true;
}

bool GuardState(eCEntity* self, char const* state)
{
    if (!life.held || life.terminal || !self || self != gCSession::GetInstance().GetHero() ||
        !gCSession::GetInstance().IsGameRunning() || persist::NativeLoading()) return false;
    Entity hero(self);
    // A module tick is not a script clock boundary: a long frame or another script can otherwise wake the hero first.
    if (DownTask(state) || !std::strcmp(state, "ZS_Unconscious_Loop")) {
        if (!std::strcmp(state, "ZS_Unconscious_Loop") && hero.Routine.GetTaskTime() > 5.0f)
            Logf("downed: stand refused player=%u sequence=%u source=native-wake timer=%.3f",
                 net::LocalId(), life.sequence, hero.Routine.GetTaskTime());
        hero.Routine.AIMode = gEAIMode_Down;
        hero.Routine.SetTaskTime(0.0f);
        if (hero.PlayerMemory.GetHitPoints() != 1) hero.PlayerMemory.SetHitPoints(1);
        return false;
    }
    Logf("downed: stand refused player=%u sequence=%u source=AI state=%s", net::LocalId(), life.sequence, state);
    hero.Routine.FullStop();
    hero.Routine.SetTask(bCString(KnockOutTask));
    hero.Routine.AIMode = gEAIMode_Down;
    hero.CharacterMovement.StopMovement();
    return true;
}

// The knocked-out hero gets up when its knock-out loop (ZS_Unconscious_Loop) finds the task time above 5 s
// and calls ContinueRoutine (static analysis). The task time is held at 0 and the hit points at most 1 (the loop
// heals +10 per second) until a partner chooses the help dialog and pays a healing potion; then it stands up
// with a quarter of its hit points. Without a potion the partner is told so and the
// hero stays down. When every known, connected partner is down or dead, the hero dies immediately. Unknown, stale,
// loading or disconnected partner activity retains the original 5-second grace; distance/world location does not.
void Tick()
{
    gCSession& session = gCSession::GetInstance();
    eCDynamicEntity* instance = session.GetHero();
    if (!instance) {
        CancelAlone("no-local-hero");
        revivedBy = 0;
        lastHp = -1;
        lethalBlowAt = 0;
        return;
    }
    if (!session.IsGameRunning()) {
        CancelAlone("local-not-playing");
        revivedBy = 0;
        return;
    }
    Entity hero(instance);
    const uint32_t now = net::Now();
    net::Remote const* remotes = net::Remotes();
    const uint8_t self = net::LocalId();
    wchar_t name[32], text[200];

    // Tell this player when someone else goes down or dies.
    for (int i = 1; i < net::MaxPlayers; ++i) {
        if (i == self) continue;
        const bool present = Playing(HelperState(remotes[i], now));
        const bool dead = present && (remotes[i].activity.flags & 4);
        const bool down = present && (remotes[i].activity.flags & 8) && !dead;
        Wide(remotes[i].latest.name, name, 32);
        if (down && !told[i]) {
            swprintf_s(text, potionNeeded ? i18n::Text(i18n::PartnerDownNeedsPotion)
                                          : i18n::Text(i18n::PartnerDown), name);
            hud::Message(hud::Warning, text);
        }
        if (dead && !toldDead[i]) {
            swprintf_s(text, i18n::Text(i18n::PartnerDied), name);
            hud::Message(hud::Bad, text);
        }
        told[i] = down;
        toldDead[i] = dead;
    }

    if (life.terminal) {
        // Terminal death is one-shot.  The native Kill script, and not a repeating task latch, owns game over.
        // Do not inspect or restart ZS_Dead/ZS_Dead_Loop/PS_Normal here: that lets PS_Normal be accepted without
        // reviving or re-running the death script, while HP remains 0 and stale revive messages stay refused.
        life.held = false;
        lethal = false;
        lethalSeenDown = false;
        nativeNoted = false;
        revivedBy = 0;
        if (hero.PlayerMemory.GetHitPoints() > 0) hero.PlayerMemory.SetHitPoints(0);
        return;
    }
    if (hero.IsDead() && !life.held) { lethal = false; revivedBy = 0; return; }
    if (persist::HeroPending()) {
        if (life.held) KeepDown(hero, "hero-restore");
        CancelAlone("hero-restore");
        return;
    }
    WatchBlows(hero, now);
    const bool down = hero.IsDown();
    if (!down && !life.held) {
        for (int i = 0; i < 2; ++i) {
            Entity item = hero.Inventory.GetItemFromSlot(i ? gESlot_LeftHand : gESlot_RightHand);
            inHand[i] = item.GetInstance() ? item.GetName().GetText() : "";
        }
    } else if (down && !wasDown && !net::IsHost()) {
        for (auto const& n : inHand)
            if (!n.empty()) items::NoteOwnDrop(n.c_str());
    }
    wasDown = down;
    if (down && lethal) lethalSeenDown = true;
    if (!down && !life.held) {
        // The knock-out task RedirectDeath sets makes the hero lie a frame or more later: not standing up yet (22.10.:
        // reset at once, every intercepted death went the game's own way and nobody could help, stages A and A2).
        if (lethal && !lethalSeenDown && now - lethalAt < 3000) return;
        lethal = false;
        lethalSeenDown = false;
        nativeNoted = false;
        CancelAlone("not-held");
        revivedBy = 0;
        if (autoHelp) Help(hero, remotes, now);
        else OfferHelp(hero, remotes, now);
        return;
    }
    if (!life.held) {
        if (!holdEnabled) return;  // QA negative control: the game's own timer stands the hero up
        // A knock-out of the game's own at 0 hit points after a blow that meant to kill: a death the game turned into a
        // knock-out; the partner helps it up, else it is one.
        if (lethalOnly && !lethal && !nativeNoted && lethalBlows && hero.PlayerMemory.GetHitPoints() <= 0 && lethalBlowAt &&
            now - lethalBlowAt < 3000 && PartnerPresent(remotes, now)) {
            lethal = true;
            lethalSeenDown = true;
            lethalAt = now;
            ++nativeLethal;
            Logf("downed: lethal blow by someone who means to kill; knocked out like a death (hp=%d)", hero.PlayerMemory.GetHitPoints());
        }
        if (lethalOnly && !lethal) {
            if (!nativeNoted) {
                nativeNoted = true;
                Logf("downed: hero knocked out by the game's own rules (not a death; hp=%d); it stands up by itself",
                     hero.PlayerMemory.GetHitPoints());
            }
            return;
        }
        Hold("lethal-knockout", "tick");
        Logf("downed: hero knocked out, waiting for a partner (hp=%d)", hero.PlayerMemory.GetHitPoints());
    }
    KeepDown(hero, "tick");

    if (revivedBy && !Able(remotes[revivedBy], now)) {
        Logf("downed: revive refused player=%u helper=%d sequence=%u reason=helper-no-longer-able",
             self, revivedBy, pendingReviveSequence);
        revivedBy = 0;
        pendingReviveSequence = 0;
    }
    if (revivedBy) {
        const int by = revivedBy;
        revivedBy = 0;
        if (life.terminal || terminalKillSent || !life.Revive(pendingReviveSequence)) return;
        pendingReviveSequence = 0;
        lethal = false;
        lethalSeenDown = false;
        lethalBlowAt = 0;
        nativeNoted = false;
        CancelAlone("paid-revive");
        ++revives;
        Transition("paid-revive", "network");
        StandUp(hero);
        Wide(remotes[by].latest.name, name, 32);
        swprintf_s(text, i18n::Text(i18n::RevivedByPartner), name);
        hud::Message(hud::Good, text);
        Logf("downed: revived by player %d (hp=%d)", by, hero.PlayerMemory.GetHitPoints());
        return;
    }

    bool helpPossible = false;
    for (int i = 1; i < net::MaxPlayers; ++i)
        if (i != self && Able(remotes[i], now)) helpPossible = true;
    const uint32_t before = aloneSince;
    const EndDecision decision = DecideEnd(helpPossible, AllOthersDown(remotes, now), now, aloneSince);
    if (before && !aloneSince) LogHelpers(&hero, "alone-cancel", now);
    if (!before && aloneSince) LogHelpers(&hero, "alone-start", now);
    if (decision == EndDecision::AllDown) {
        Die(hero, "all-down");
        return;
    }
    if (decision == EndDecision::AloneGrace) {
        LogHelpers(&hero, "alone-expiry", now);
        Die(hero, "alone-grace");
    }
}

// A partner paid a potion for us (WorldState kind 8 to this player).
void Revived(int byPlayer, uint32_t sequence)
{
    gCSession& session = gCSession::GetInstance();
    eCDynamicEntity* instance = session.GetHero();
    const bool helper = byPlayer > 0 && byPlayer < net::MaxPlayers && byPlayer != net::LocalId() &&
                        Able(net::Remotes()[byPlayer], net::Now());
    if (!helper || !life.held || !sequence || sequence != life.sequence || life.terminal || terminalKillSent ||
        !instance || !session.IsGameRunning() || persist::HeroPending()) {
        Logf("downed: revive refused player=%u helper=%d sequence=%u current=%u held=%d terminal=%d reason=stale-or-ineligible",
             net::LocalId(), byPlayer, sequence, life.sequence, life.held ? 1 : 0, life.terminal ? 1 : 0);
        return;
    }
    revivedBy = byPlayer;
    pendingReviveSequence = sequence;
    Logf("downed: revive accepted player=%u helper=%d sequence=%u source=network", net::LocalId(), byPlayer, sequence);
}

// A partner stood next to us without a healing potion (kind 9).
void HelperHasNoPotion(int byPlayer)
{
    if (byPlayer <= 0 || byPlayer >= net::MaxPlayers || !life.held) return;
    wchar_t name[32], text[160];
    Wide(net::Remotes()[byPlayer].latest.name, name, 32);
    swprintf_s(text, i18n::Text(i18n::HelperHasNoPotion), name);
    hud::Message(hud::Info, text);
}

bool Held() { return life.held; }

bool CombatUnavailable(eCEntity* entity)
{
    if (!entity) return false;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (hero && (entity == hero || entity == remote::ControlledBody())) {
        Entity player(entity);
        return CombatUnavailable(true, life.held, life.terminal, player.IsDown(), player.IsDead(), 0);
    }
    const int id = remote::PuppetOwner(entity);
    if (!id) return false;
    net::Remote const& r = net::Remotes()[id];
    // Latest accepted player state is authority. Native Down on the puppet is an output of this rule,
    // not an input: otherwise publishing Down would latch it forever after the owner revives.
    return r.connected && r.latestTicks && r.latest.world == remote::CurrentWorld() &&
           CombatUnavailable(true, false, false, false, false, r.latest.flags);
}

uint32_t UnavailablePlayers()
{
    uint32_t players = 0;
    const unsigned local = net::LocalId();
    eCEntity* hero = gCSession::GetInstance().GetHero();
    eCEntity* controlled = hero ? remote::ControlledBody() : nullptr;
    if (local && local < net::MaxPlayers && (CombatUnavailable(hero) || CombatUnavailable(controlled)))
        players |= uint32_t{1} << local;
    for (unsigned id = 1; id < net::MaxPlayers; ++id)
        if (id != local && CombatUnavailable(remote::PuppetOf(id))) players |= uint32_t{1} << id;
    return players;
}

void LogCombatDecision(eCEntity* npc, eCEntity* player, char const* decision, bool transition)
{
    static unsigned decisions = 0;
    static uint32_t lastLog = 0;
    const uint32_t now = net::Now();
    ++decisions;
    if (!transition && decisions > 40 && now - lastLog < 5000) return;
    lastLog = now;
    const int remoteId = remote::PuppetOwner(player);
    const unsigned id = remoteId ? static_cast<unsigned>(remoteId) : net::LocalId();
    const uint8_t flags = remoteId ? net::Remotes()[remoteId].latest.flags : 0;
    const uint32_t sequence = remoteId ? net::Remotes()[remoteId].latest.downedSequence : life.sequence;
    const uint32_t age = remoteId ? now - net::Remotes()[remoteId].latestTicks : 0;
    const bool hasNpc = npc && npc->GetPropertySet(eEPropertySetType_NPC);
    const bool hasRoutine = npc && npc->GetPropertySet(eEPropertySetType_ScriptRoutine);
    Entity actor(npc);
    Logf("DOWNEDTARGET role=%s player=%u episode=%u flags=%u age=%u excluded=%#x held=%d terminal=%d decision=%s npc=%s ptr=%p ai=%d task=%s state=%s",
         net::IsHost() ? "host" : "client", id, sequence, flags, age, UnavailablePlayers(),
         !remoteId && life.held ? 1 : 0, !remoteId && life.terminal ? 1 : 0, decision,
         npc ? actor.GetName().GetText() : "-", npc, hasRoutine ? static_cast<int>(actor.Routine.AIMode) : -1,
         hasRoutine ? actor.Routine.GetCurrentTask().GetText() : "-",
         hasRoutine ? actor.Routine.GetCurrentState().GetText() : "-");
    if (hasNpc)
        Logf("DOWNEDBIND npc=%p player=%p current=%p alternative=%p attacker=%p last=%p spell=%p alignment=%p inflictor=%p combat=%u",
             npc, player, actor.NPC.GetCurrentTarget().GetInstance(), actor.NPC.GetAlternativeTarget().GetInstance(),
             actor.NPC.GetCurrentAttacker().GetInstance(), actor.NPC.GetLastAttacker().GetInstance(),
             actor.NPC.GetSpellTarget().GetInstance(), actor.GetAlignmentTarget().GetInstance(),
             npc->GetPropertySet(eEPropertySetType_DamageReceiver) ? actor.DamageReceiver.GetLastInflictor().GetInstance() : nullptr,
             static_cast<unsigned>(actor.NPC.CombatState));
}

uint32_t Sequence() { return life.held ? life.sequence : 0; }

void Restore(bool waiting, char const* source)
{
    if (life.terminal || terminalKillSent) return;
    if (waiting && !life.held) {
        Hold("saved-downed-life", source);
        lethal = true;
    } else if (life.held) {
        Transition("preserve-downed-life", source);
    }
}

void NoteBlow()
{
    lethalBlowAt = net::Now() ? net::Now() : 1;
}

// Not a death the mod turned into a knock-out (that one waits for help, and nobody robs it): RedirectDeath sets lethal
// before the hero lies down.
bool NativeDown()
{
    eCDynamicEntity* instance = gCSession::GetInstance().GetHero();
    if (!instance || life.held || lethal || life.terminal || !gCSession::GetInstance().IsGameRunning()) return false;
    Entity hero(instance);
    return hero.IsDown() && !hero.IsDead();
}

void OnWorldChange()
{
    life.OnWorldChange(); // old-world paid revives must not apply after the load
    lethal = life.held;
    lethalSeenDown = false;
    lethalAt = 0;
    lastHp = -1;
    lethalBlowAt = 0;
    nativeNoted = false;
    terminalKillSent = false;
    crashlog::EndStep("world-change");
    Transition(life.held ? "carry-downed-life" : "reset-terminal-life", "world-change");
    wasDown = false;
    CancelAlone("world-change");
    revivedBy = 0;
    pendingReviveSequence = 0;
    for (int i = 0; i < net::MaxPlayers; ++i) {
        told[i] = false;
        toldDead[i] = false;
        helpingSince[i] = 0;
        hinted[i] = false;
        paidAt[i] = 0;
        paidSequence[i] = 0;
    }
    for (int i = 0; i < 2; ++i) inHand[i].clear();
}

void Off(LogFn log)
{
    enabled = false;
    log("DOWNEDOFF ok=1");
}

void HoldOff(LogFn log)
{
    holdEnabled = false;
    log("HOLDOFF ok=1");
}

void PotionOff(LogFn log)
{
    potionNeeded = false;
    log("POTIONOFF helping up is free");
}

void LethalOnlyOff(LogFn log)
{
    lethalOnly = false;
    log("LETHALONLYOFF ok=1");
}

// QA: items for the local hero ("Template" or "Template-Amount"; QA arguments allow only [A-Za-z0-9_-]).
void GivePotion(char const* argument, LogFn log)
{
    char line[200], templ[64] = {};
    strncpy_s(templ, argument, _TRUNCATE);
    int amount = 1;
    if (char* dash = std::strrchr(templ, '-')) {
        *dash = '\0';
        amount = std::atoi(dash + 1);
    }
    eCDynamicEntity* instance = gCSession::GetInstance().GetHero();
    const Template item(templ);
    if (!instance || !item.IsValid()) { Format(line, "GIVEPOTION name=%s valid=%d", templ, item.IsValid() ? 1 : 0); log(line); return; }
    Entity hero(instance);
    if (amount > 0) {
        hero.Inventory.CreateItems(item, 0, amount);
    } else {  // "-0": take all of them away
        for (GEInt i = hero.Inventory.GetStackCount() - 1; i >= 0; --i) {
            Entity carried = hero.Inventory.GetTemplateItem(i);
            if (carried.GetInstance() && !std::strcmp(carried.GetName().GetText(), templ)) hero.Inventory.DeleteItems(i, hero.Inventory.GetAmount(i));
        }
    }
    Format(line, "GIVEPOTION name=%s valid=1 amount=%d now=%d", templ, amount, hero.Inventory.GetItemAmount(item));
    log(line);
}

// QA: the healing potions the local hero carries.
void Potions(LogFn log)
{
    char line[300];
    eCDynamicEntity* instance = gCSession::GetInstance().GetHero();
    if (!instance) { log("POTIONS end count=0"); return; }
    Entity hero(instance);
    int total = 0;
    for (GEInt i = 0; i < hero.Inventory.GetStackCount(); ++i) {
        Entity item = hero.Inventory.GetTemplateItem(i);
        if (!item.GetInstance() || std::strncmp(item.GetName().GetText(), "It_Potion", 9) != 0) continue;
        const GEInt amount = hero.Inventory.GetAmount(i);
        if (!std::strncmp(item.GetName().GetText(), PotionPrefix, std::strlen(PotionPrefix))) total += amount;
        Format(line, "POTION name=%s amount=%d gold=%d", item.GetName().GetText(), amount, item.Item.GetGoldValue());
        log(line);
    }
    Format(line, "POTIONS end health=%d", total);
    log(line);
}

void Report(LogFn log)
{
    char line[240];
    Format(line, "DOWNED lethal=%d lethal_only=%d lethal_blows=%d native_lethal=%u enabled=%d hold=%d held=%d redirects=%u revives=%u releases=%u deaths=%u potions_paid=%u no_potion=%u potion_needed=%d"
                    " auto_help=%d help_option=%d offered=%u chosen=%u",
              lethal ? 1 : 0, lethalOnly ? 1 : 0, lethalBlows ? 1 : 0, nativeLethal, enabled ? 1 : 0, holdEnabled ? 1 : 0, life.held ? 1 : 0, redirects, revives, releases, deaths, potionsPaid, refusedNoPotion,
              potionNeeded ? 1 : 0, autoHelp ? 1 : 0, helpOption ? 1 : 0, optionsOffered, optionsChosen);
    log(line);
}

void AutoHelpOn(LogFn log)
{
    autoHelp = true;
    log("AUTOHELPON ok");
}

void HelpOptionOff(LogFn log)
{
    helpOption = false;
    log("HELPOPTIONOFF ok");
}
void HelpLineSaid() { helpSaidAt = net::Now() ? net::Now() : 1; }

// QA negative controls of the play-evening fixes (27.10.): each switches its fix off (on: clientfameon).
bool Switch(char const* action, LogFn log)
{
    if (!std::strcmp(action, "helplineoff")) { helpLineOnly = false; log("SWITCH helplineoff ok=1"); return true; }
    if (!std::strcmp(action, "autohelpoff")) { autoHelp = false; log("SWITCH autohelpoff ok=1"); return true; }
    if (!std::strcmp(action, "lethalblowsoff")) { lethalBlows = false; log("SWITCH lethalblowsoff ok=1"); return true; }
    return false;
}
}
