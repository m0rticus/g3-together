// SPDX-License-Identifier: GPL-3.0-or-later
#include "downed.h"
#include "downed_terminal.h"
#include "player_immunity.h"
#include "ai.h"
#include "body.h"
#include "crashlog.h"
#include "hud.h"
#include "i18n.h"
#include "items.h"
#include "log.h"
#include "persist.h"
#include "remote.h"
#include "movement.h"
#include "telemetry.h"
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <intrin.h>

namespace g3t::downed {
namespace {
auto& recovery = life.recovery;
auto& convergence = life.convergence;
auto& lastHp = life.lastHp;
auto& lethalBlowAt = life.lethalBlowAt;
auto& recoveryUnknownNoted = life.recoveryUnknownNoted;
auto& potionsPaid = life.potionsPaid;
auto& refusedNoPotion = life.refusedNoPotion;
auto& helpSaidAt = life.helpSaidAt;
auto& helpNotSaid = life.helpNotSaid;
auto& lastOffer = life.lastOffer;
auto& optionsOffered = life.optionsOffered;
auto& optionsChosen = life.optionsChosen;
}
PeerRecovery puppetRecovery[net::MaxPlayers];
InteractionProtection interaction;
eCEntity* interactionHero = nullptr;
bool interactionWorldActive = true;
uint8_t lastInteractionFlags = 0;
namespace {
constexpr float ReviveRange = 250.0f;
constexpr uint32_t StaleState = 5000;
char const* const PotionPrefix = "It_Potion_Health";
char const* const HelpLine = "INFO_SKALVERAM320060309153144";
bool Present(net::Remote const& r, uint32_t now) {
    return r.connected && r.latestTicks && now - r.latestTicks < StaleState && (r.latest.flags & 1) &&
           r.latest.world == remote::CurrentWorld() && net::InWorld(r.latest.id);
}
}

int NativeInteractionPage(gCGUIManager* manager)
{
    if (!manager) return -1;
    static bool checked = false, supported = false;
    if (!checked) {
        HMODULE game = GetModuleHandleA("Game.dll");
        if (!game) return -1;
        checked = true;
        auto* code = reinterpret_cast<unsigned char*>(game);
        static unsigned char const pageBytes[] = {
            0x8b, 0x41, 0x08, 0x85, 0xc0, 0x74, 0x11, 0x83, 0x78, 0x48, 0xff, 0x0f, 0x95, 0xc0,
            0x84, 0xc0, 0x74, 0x06, 0xb8, 0x11, 0x00, 0x00, 0x00, 0xc3, 0x8b, 0x49, 0x04,
            0x85, 0xc9, 0x74, 0x07, 0x8b, 0x81, 0x84, 0x00, 0x00, 0x00, 0xc3, 0x83, 0xc8, 0xff, 0xc3
        };
        static unsigned char const tutorialSelect[] = {0x83, 0xff, 0x0f};
        static unsigned char const tutorialSave[] = {
            0x8b, 0x85, 0x84, 0x00, 0x00, 0x00, 0x89, 0x85, 0x88, 0x00, 0x00, 0x00
        };
        supported = GetProcAddress(game, "?GetActivePage@gCGUIManager@@QBE?AW4gEGamePage@@XZ") ==
                        reinterpret_cast<FARPROC>(code + 0x7b490) &&
                    !std::memcmp(code + 0x7b490, pageBytes, sizeof(pageBytes)) &&
                    !std::memcmp(code + 0x8b359, tutorialSelect, sizeof(tutorialSelect)) &&
                    !std::memcmp(code + 0x8b384, tutorialSave, sizeof(tutorialSave));
        Log(supported ? "downed: native interaction pages ready (dialog/trade beneath overlays and tutorial)"
                      : "downed: ANOMALY interaction page layout mismatch; overlay protection unavailable");
    }
    if (!supported) return static_cast<int>(manager->GetActivePage());
    // GetActivePage masks the main page with secondary overlays; tutorial saves it at +0x88.
    void* root = nullptr;
    std::memcpy(&root, reinterpret_cast<unsigned char*>(manager) + 4, sizeof(root));
    if (!root) return -1;
    int page = -1, beforeTutorial = -1;
    std::memcpy(&page, static_cast<unsigned char*>(root) + 0x84, sizeof(page));
    if (page == gEGamePage_Tutorial)
        std::memcpy(&beforeTutorial, static_cast<unsigned char*>(root) + 0x88, sizeof(beforeTutorial));
    return InteractionPage(page, beforeTutorial);
}

uint8_t InteractionOf(eCEntity* entity)
{
    if (!entity) return 0;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (hero && (entity == hero || entity == remote::ControlledBody())) return LocalInteractionFlags();
    const int id = remote::PuppetOwner(entity);
    if (id <= 0 || id >= net::MaxPlayers || !interactionWorldActive ||
        persist::NativeLoading() || persist::HeroPending() || ai::ShuttingDown()) return 0;
    return PlayerInteractionFlags(static_cast<unsigned>(id));
}

bool TrackPuppetRecovery(int id)
{
    net::Remote const& r = net::Remotes()[id];
    PeerRecovery& peer = puppetRecovery[id];
    const bool wasHeld = peer.held, wasGrace = peer.recovery.grace;
    const uint32_t now = net::Now();
    const bool present = r.connected && r.latestTicks && r.latest.world == remote::CurrentWorld();
    peer.Observe(present, r.latest.flags,
                 RecoveryStanding(r.latest.aniState, (r.latest.flags & 2) != 0, r.latest.animation), now);
    if (wasHeld && !peer.held && peer.recovery.Protected(now))
        Logf("downed: puppet recovery begin player=%d flags=%u motion=%s", id, r.latest.flags, r.latest.animation);
    if (!wasGrace && peer.recovery.grace)
        Logf("downed: puppet recovery standing player=%d grace_ms=%u motion=%s", id, ReviveGraceMs, r.latest.animation);
    return peer.recovery.Protected(now);
}

void Wide(char const* text, wchar_t* out, int size)
{
    out[0] = 0;
    if (!MultiByteToWideChar(CP_UTF8, 0, text, -1, out, size)) out[0] = 0;
    out[size - 1] = 0;
}

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

void PayAndHelp(Entity& hero, int i, net::Remote const& r, uint32_t now)
{
    const bCVector& at = hero.GetInstance()->GetWorldPosition();
    const float here[3] = {at.GetX(), at.GetY(), at.GetZ()};
    if (life.held || life.terminal || hero.IsDown() || hero.IsDead() || i == net::LocalId() || !Present(r, now) ||
        !HeldPuppet(r.latest.flags, r.latest.downedSequence) ||
        body::Distance(r.latest.position, here) > ReviveRange ||
        !peers[i].payment.NeedsPayment(r.latest.downedSequence)) {
        Logf("downed: help refused player=%u target=%d sequence=%u reason=not-eligible", net::LocalId(), i, r.latest.downedSequence);
        return;
    }
    wchar_t name[32], text[200];
    Wide(r.latest.name, name, 32);
    char potion[64] = "";
    const GEInt stack = SmallestPotion(hero, potion, sizeof(potion));
    if (stack < 0) {
        if (!peers[i].hinted) {
            peers[i].hinted = true;
            ++refusedNoPotion;
            swprintf_s(text, i18n::Text(i18n::NeedHealingPotion), name);
            hud::Message(hud::Warning, text);
            SendToPlayer(9, i, r.latest.downedSequence);
            telemetry::OnDownedEvent("help_no_potion", static_cast<uint8_t>(i), r.latest.downedSequence, true, false, hero.PlayerMemory.GetHitPoints(), nullptr, net::LocalId(), "missing_potion");
            Logf("downed: wants to help player %d without a healing potion", i);
        }
        return;
    }
    if (stack >= 0) {
        if (!hero.Inventory.DeleteItems(stack, 1)) {
            Logf("downed: help refused player=%u target=%d sequence=%u reason=potion-removal-failed potion=%s",
                 net::LocalId(), i, r.latest.downedSequence, potion);
            return;
        }
        ++potionsPaid;
    }
    peers[i].payment.MarkPaid(r.latest.downedSequence);
    peers[i].hinted = false;
    SendToPlayer(8, i, r.latest.downedSequence);
    NoteReviveSent(i, r.latest.downedSequence);
    telemetry::OnDownedEvent("help_paid_potion", static_cast<uint8_t>(i), r.latest.downedSequence, true, false, hero.PlayerMemory.GetHitPoints(), nullptr, net::LocalId(), *potion ? potion : "potion");
    swprintf_s(text, i18n::Text(i18n::HelpedUpWithPotion), name);
    hud::Message(hud::Good, text);
    Logf("downed: helped player %d up, paid %s sequence=%u source=help-line", i, *potion ? potion : "nothing", r.latest.downedSequence);
}

void RebuildOwnerMap(gCInfoManager_PS* manager)
{
    using Build = void(__thiscall*)(gCInfoManager_PS*);
    static Build build = reinterpret_cast<Build>(GetProcAddress(GetModuleHandleA("Game.dll"), "?BuildMapInfosByOwners@gCInfoManager_PS@@AAEXXZ"));
    if (build) build(manager);
    else Log("downed: BuildMapInfosByOwners not found; the help option will not show");
}

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
        const bool said = (helpSaidAt && now - helpSaidAt < 5000) || (helpInfo && helpInfo->AccessInfoGiven());
        if (ours && !peers[i].helpStarted && !said) ++helpNotSaid;
        if (ours && !peers[i].helpStarted && said) {
            helpSaidAt = 0;
            peers[i].helpStarted = true;
            ++optionsChosen;
            Logf("downed: the player chose to help player %d up", i);
            peers[i].hinted = false;  // every choice without a potion says so again
            PayAndHelp(hero, i, remotes[i], now);
        }
        if (!ours) peers[i].helpStarted = false;
        if (!scan) continue;
        net::Remote const& r = remotes[i];
        const bool waiting = Present(r, now) && HeldPuppet(r.latest.flags, r.latest.downedSequence) &&
                             peers[i].payment.NeedsPayment(r.latest.downedSequence);
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

uint8_t LocalInteractionFlags()
{
    gCSession& session = gCSession::GetInstance();
    eCEntity* hero = session.GetHero();
    if (hero != interactionHero) {
        interaction.Clear();
        interactionHero = hero;
    }
    const uint32_t now = net::Now();

    const uint8_t before = lastInteractionFlags;
    const bool eligible = interactionWorldActive && hero && session.IsGameRunning() &&
                          !ai::ShuttingDown() && !persist::NativeLoading() && !persist::HeroPending() &&
                          !life.held && !life.terminal;
    if (!eligible) {
        interaction.Clear();
    } else {
        Entity player(hero);
        const bool alive = !player.IsDead() && !player.IsDown();
        const int page = alive ? NativeInteractionPage(session.GetGUIManager()) : -1;
        gCInfoManager_PS* infos = alive ? gCInfoManager_PS::GetInstance() : nullptr;
        eCEntity* partner = infos && infos->AccessIsRunning() && !infos->AccessEndRequested() &&
                           infos->GetDialogPartner1() == hero ? infos->GetDialogPartnerNPC() : nullptr;
        const bool spoken = partner && partner != hero &&
                            partner->GetPropertySet(eEPropertySetType_NPC) && !Entity(partner).IsDead();
        const bool dialogue = DialogueObserved(page, infos && infos->AccessIsRunning(),
                                              infos && infos->AccessEndRequested(),
                                              infos && infos->GetDialogPartner1() == hero, spoken);
        interaction.Observe(alive, dialogue, page == gEGamePage_Trade, now);
    }
    const uint8_t flags = interaction.Flags(now);
    if (before != flags)
        Logf("PROTECTION role=%s player=%u reason=%s previous=%s flags=%u grace_ms=%u",
             net::IsHost() ? "host" : "client", net::LocalId(), InteractionReason(flags),
             InteractionReason(before), flags, (flags & InteractionGrace) ? InteractionGraceMs : 0);
    lastInteractionFlags = flags;
    return flags;
}

uint8_t PlayerInteractionFlags(unsigned player)
{
    if (!player || player >= net::MaxPlayers) return 0;
    if (player == net::LocalId()) return LocalInteractionFlags();
    if (!interactionWorldActive || ai::ShuttingDown() || persist::NativeLoading() ||
        persist::HeroPending() || !gCSession::GetInstance().IsGameRunning()) return 0;
    net::Remote const& r = net::Remotes()[player];
    return PeerInteraction(r.connected, r.activity.known, r.activity.ticks, r.activity.world,
                           remote::CurrentWorld(), r.activity.flags, net::Now());
}

bool HarmProtected(eCEntity* entity)
{
    if (!entity || ai::ShuttingDown() || persist::NativeLoading() || persist::HeroPending() ||
        !gCSession::GetInstance().IsGameRunning()) return false;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (hero && (entity == hero || entity == remote::ControlledBody()))
        return HarmImmune(true, life.held, life.terminal, 0, recovery.Protected(net::Now()), LocalInteractionFlags() != 0);
    const int id = remote::PuppetOwner(entity);
    if (id <= 0 || id >= net::MaxPlayers) return false;
    net::Remote const& r = net::Remotes()[id];
    const bool present = r.connected && r.latestTicks && r.latest.world == remote::CurrentWorld();
    return HarmImmune(present, false, false, r.latest.flags, TrackPuppetRecovery(id)) || InteractionOf(entity) != 0;
}

void LogGroundEvent(eCEntity* victim, eCEntity* attacker, char const* path, int before, int after, char const* decision)
{
    if (!victim || ai::ShuttingDown()) return;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    if (!hero || (victim != hero && victim != remote::ControlledBody())) return;
    Entity player(victim);
    const bCString task = player.Routine.GetCurrentTask(), state = player.Routine.GetCurrentState();
    const int pose = static_cast<int>(player.Routine.AniState);
    const uint32_t now = net::Now();
    const uint8_t interactionFlags = LocalInteractionFlags();
    if (!interactionFlags && !life.held && !life.terminal && !recovery.Protected(now) && !(pose >= 15 && pose <= 21) &&
        !HarmfulReaction(task.GetText()) && !HarmfulReaction(state.GetText()) &&
        !std::strstr(task.GetText(), "Fall") && !std::strstr(state.GetText(), "Fall")) return;
    Logf("GROUNDHIT role=%s player=%u episode=%u path=%s attacker=%s hp=%d->%d held=%d terminal=%d "
         "rising=%d grace_ms=%u ai=%d ani=%d task=%s state=%s decision=%s protection=%s",
         net::IsHost() ? "host" : "client", net::LocalId(), life.sequence, path,
         attacker ? Entity(attacker).GetName().GetText() : "-", before, after,
         life.held ? 1 : 0, life.terminal ? 1 : 0, recovery.rising ? 1 : 0,
         recovery.grace && now - recovery.stoodAt < ReviveGraceMs ? ReviveGraceMs - (now - recovery.stoodAt) : 0,
         static_cast<int>(player.Routine.AIMode), pose, task.GetText(), state.GetText(), decision,
         InteractionReason(interactionFlags));
    telemetry::OnGroundHit(victim, attacker, path, before, after, task.GetText(), InteractionReason(interactionFlags), decision, life.sequence);
}

bool RejectHarm(eCEntity* victim, eCEntity* attacker, char const* path)
{
    if (!HarmProtected(victim)) return false;
    Entity player(victim);
    const int hp = victim->GetPropertySet(eEPropertySetType_PlayerMemory)
        ? player.PlayerMemory.GetHitPoints() : static_cast<int>(player.DamageReceiver.HitPoints);
    LogGroundEvent(victim, attacker, path, hp, hp, "reject-before-effects");
    if (const uint8_t flags = InteractionOf(victim)) {
        static unsigned refused = 0;
        static uint32_t lastLog = 0;
        const uint32_t now = net::Now();
        if (++refused <= 40 || now - lastLog >= 5000) {
            lastLog = now;
            const int id = remote::PuppetOwner(victim);
            Logf("PROTECTEDHIT role=%s player=%u path=%s reason=%s hp=%d decision=reject-before-effects",
                 net::IsHost() ? "host" : "client", id ? static_cast<unsigned>(id) : net::LocalId(),
                 path, InteractionReason(flags), hp);
        }
    }
    if (victim->GetPropertySet(eEPropertySetType_DamageReceiver)) player.DamageReceiver.DamageAmount = 0;
    return true;
}

bool RejectReaction(eCEntity* victim, char const* state, char const* path)
{
    if (!victim || !HarmfulReaction(state) || !HarmProtected(victim)) return false;
    // The held task itself and its native loop must still establish/maintain the lying pose.
    if (life.held && victim == gCSession::GetInstance().GetHero() && DownTask(state)) {
        if (!convergence.settled) return false;
        convergence.Request();
        return true;
    }
    eCEntity* attacker = Entity(victim).NPC.GetCurrentAttacker().GetInstance();
    if (!RejectHarm(victim, attacker, path)) return false;
    Logf("downed: ground reaction refused player=%u path=%s requested=%s", net::LocalId(), path, state);
    return true;
}

void ResetPeerImmunity(unsigned player) { if (player < net::MaxPlayers) puppetRecovery[player].Clear(); }
void HelpLineSaid() { helpSaidAt = net::Now() ? net::Now() : 1; }

void StandUp(Entity& hero)
{
    const int max = hero.PlayerMemory.GetHitPointsMax();
    hero.Routine.FullStop();
    hero.PlayerMemory.SetHitPoints(max / 4 > 1 ? max / 4 : 1);
    hero.Routine.ContinueRoutine();  // like the knock-out loop itself: Rtn_Player stands the hero up
}

void WatchBlows(Entity& hero, uint32_t now)
{
    const int hp = hero.PlayerMemory.GetHitPoints();
    const int before = lastHp;
    lastHp = hp;
    if (before < 0 || hp >= before || !net::Connected()) return;
    eCEntity* attacker = hero.NPC.GetCurrentAttacker().GetInstance();
    if (!attacker || remote::IsPuppet(attacker) || !attacker->GetPropertySet(eEPropertySetType_NPC)) return;
    if (IsLethalNpcAttacker(attacker, hero)) lethalBlowAt = now ? now : 1;
    else lethalBlowAt = 0;
}
}
