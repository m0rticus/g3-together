// SPDX-License-Identifier: GPL-3.0-or-later
#include "telemetry_internal.h"

#include "guid.h"
#include "npc_item_quality_policy.h"
#include "npclease_runtime.h"
#include "npcs.h"
#include "remote.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace g3t::telemetry {

static_assert(gESlot_Helmet < schema::MaxTrackedSlots, "MaxTrackedSlots must cover all native gESlot values");

namespace detail {

CombatContext recentJudge;
schema::CausalStamp lastCombatCause;
schema::CausalStamp lastCrimeCause;
schema::CausalStamp lastLootCause;
schema::CausalStamp lastQuestCause;
schema::GroundHitFilter groundFilter;

namespace {

char const* EntityKindTag(eCEntity const* entity) noexcept
{
    if (!entity) return "none";
    if (entity == gCSession::GetInstance().GetHero()) return "hero";
    if (entity == remote::ControlledBody()) return "controlled";
    if (remote::IsPuppet(entity)) return "puppet";
    auto* mut = const_cast<eCEntity*>(entity);
    if (mut->GetPropertySet(eEPropertySetType_NPC)) return npcs::IsMirrored(mut) ? "mirror_npc" : "sim_npc";
    if (mut->GetPropertySet(eEPropertySetType_Item)) return "item";
    if (mut->GetPropertySet(eEPropertySetType_Inventory)) return "container";
    if (mut->GetPropertySet(eEPropertySetType_Interaction)) return "interact";
    return "entity";
}

int EntityHp(eCEntity* entity) noexcept
{
    if (!entity) return -1;
    Entity e(entity);
    if (entity->GetPropertySet(eEPropertySetType_PlayerMemory)) return e.PlayerMemory.GetHitPoints();
    if (entity->GetPropertySet(eEPropertySetType_DamageReceiver)) return static_cast<int>(e.DamageReceiver.HitPoints);
    return -1;
}

int EntityHpMax(eCEntity* entity) noexcept
{
    if (!entity) return -1;
    Entity e(entity);
    if (entity->GetPropertySet(eEPropertySetType_PlayerMemory)) return e.PlayerMemory.GetHitPointsMax();
    if (entity->GetPropertySet(eEPropertySetType_DamageReceiver)) return static_cast<int>(e.DamageReceiver.HitPointsMax);
    return -1;
}

uint8_t OwnerPlayerOf(eCEntity* entity) noexcept
{
    if (!entity) return 0;
    if (entity == gCSession::GetInstance().GetHero() || entity == remote::ControlledBody()) return net::LocalId();
    if (const int p = remote::PuppetOwner(entity)) return static_cast<uint8_t>(p);
    if (const uint8_t r = npclease::RemoteOwner(entity)) return r;
    if (!net::IsHost() && npclease::LocalAuthority(entity)) return net::LocalId();
    return net::IsHost() ? 1u : (npcs::IsMirrored(entity) ? 1u : 0u);
}

} // namespace

char const* SafeEntityName(eCEntity const* entity) noexcept
{
    if (!entity) return "-";
    char const* raw = entity->GetName().GetText();
    return (raw && *raw) ? raw : "(unnamed)";
}

void AppendEntity(KvWriter& kv, char const* prefix, eCEntity* entity,
                  uint8_t const fallbackGuid[16], char const* fallbackName)
{
    char kGuid[32], kKind[32], kOwner[32], kHp[32], kTask[32], kPos[32];
    Format(kGuid, "%s_guid", prefix);
    Format(kKind, "%s_kind", prefix);
    Format(kOwner, "%s_owner", prefix);
    Format(kHp, "%s_hp", prefix);
    Format(kTask, "%s_task", prefix);
    Format(kPos, "%s_pos", prefix);
    if (!entity && fallbackGuid) entity = guid::Resolve(fallbackGuid).GetInstance();
    char guidText[40] = "-";
    if (entity) guid::FormatEntity(entity, guidText, sizeof(guidText));
    else if (fallbackGuid) guid::Format(fallbackGuid, guidText, sizeof(guidText));
    char const* displayName = entity ? SafeEntityName(entity) : ((fallbackName && *fallbackName) ? fallbackName : "-");
    kv.Str(prefix, displayName).Str(kGuid, guidText).Str(kKind, EntityKindTag(entity));
    if (!entity) return;
    kv.Uint(kOwner, OwnerPlayerOf(entity));
    if (const int hp = EntityHp(entity); hp >= 0) {
        char hpBuf[32];
        Format(hpBuf, "%d/%d", hp, EntityHpMax(entity));
        kv.Str(kHp, hpBuf);
    }
    if (auto* r = static_cast<gCScriptRoutine_PS*>(entity->GetPropertySet(eEPropertySetType_ScriptRoutine)))
        kv.Str(kTask, r->GetCurrentTask().GetText());
    const bCVector& p = entity->GetWorldPosition();
    kv.Pos(kPos, p.GetX(), p.GetY(), p.GetZ());
}

float DistanceMeters(eCEntity const* a, eCEntity const* b) noexcept
{
    if (!a || !b) return -1.0f;
    const bCVector& pa = a->GetWorldPosition();
    const bCVector& pb = b->GetWorldPosition();
    const float dx = pa.GetX() - pb.GetX(), dy = pa.GetY() - pb.GetY(), dz = pa.GetZ() - pb.GetZ();
    return std::sqrt(dx * dx + dy * dy + dz * dz) * 0.01f;
}

int CountHeroGold(Entity& hero) noexcept
{
    int total = 0;
    for (GEInt i = 0; i < hero.Inventory.GetStackCount(); ++i) {
        if (hero.Inventory.GetSlotFromStack(i) != gESlot_None) continue;
        Entity item = hero.Inventory.GetTemplateItem(i);
        if (item.GetInstance() && std::strcmp(item.GetName().GetText(), "It_Gold") == 0)
            total += hero.Inventory.GetAmount(i);
    }
    return total;
}

char const* CrimeLabel(int crime) noexcept
{
    if (crime < 0) return "-";
    if (crime == gEPlayerCrime_None) return "None";
    if (crime == gEPlayerCrime_Livestock) return "Livestock";
    if (crime == gEPlayerCrime_Attack) return "Attack";
    if (crime == gEPlayerCrime_Theft) return "Theft";
    if (crime == gEPlayerCrime_Murder) return "Murder";
    return "Other";
}

char const* ReasonLabel(int reason) noexcept
{
    if (reason == gEAttackReason_None) return "None";
    if (reason == gEAttackReason_Livestock) return "Livestock";
    if (reason == gEAttackReason_Theft) return "Theft";
    if (reason == gEAttackReason_Murder) return "Murder";
    if (reason == gEAttackReason_UseInteractObject) return "UseInteractObject";
    if (reason == gEAttackReason_PlayerCommand) return "PlayerCommand";
    if (reason == gEAttackReason_ReactToDamage) return "ReactToDamage";
    if (reason == gEAttackReason_Enemy) return "Enemy";
    if (reason == gEAttackReason_Prey) return "Prey";
    if (reason == gEAttackReason_Kill) return "Kill";
    if (reason == gEAttackReason_Angry) return "Angry";
    if (reason == gEAttackReason_ReactToWeapon) return "ReactToWeapon";
    if (reason == gEAttackReason_Duel) return "Duel";
    if (reason == gEAttackReason_Arena) return "Arena";
    if (reason == gEAttackReason_Revolution) return "Revolution";
    return "Reason";
}

char const* OutcomeLabel(uint8_t outcome) noexcept
{
    return outcome == net::HitKnockOut ? "knockout" : (outcome == net::HitDeath ? "death" : "damage");
}

char const* ReactionLabel(uint8_t reaction) noexcept
{
    switch (reaction) {
    case net::HitQuickStumble: return "quick_stumble";
    case net::HitStumble: return "stumble";
    case net::HitPierceStumble: return "pierce_stumble";
    case net::HitSitKnockDown: return "knockdown";
    default: return "none";
    }
}

} // namespace detail

namespace {

using detail::AppendEntity;
using detail::CountHeroGold;
using detail::CrimeLabel;
using detail::Event;
using detail::groundFilter;
using detail::lastCombatCause;
using detail::lastCrimeCause;
using detail::lastLootCause;
using detail::lastQuestCause;
using detail::NowMs;
using detail::recentJudge;
using detail::SafeEntityName;
using schema::PeerSnap;
using schema::SlotTable;
using schema::Vitals;

bool worldBaselineEmitted = false;
uint32_t baselineEpoch = 0;
Vitals prevVitals;
SlotTable prevSlots;
bool wasDialogOpen = false;
char dialogPartnerName[64] = {};
char dialogPartnerGuid[40] = {};
PeerSnap prevPeers[net::MaxPlayers] = {};

struct CarryEntry {
    char name[64] = {};
    int amount = 0;
};

thread_local TransferScope* activeTransfer = nullptr;
thread_local std::vector<CarryEntry> activeHeroCarryBefore;
thread_local std::vector<CarryEntry> activeHeroCarryAfter;

char const* SlotLabel(int slot) noexcept
{
    if (slot == gESlot_RightHand) return "RightHand";
    if (slot == gESlot_LeftHand) return "LeftHand";
    if (slot == gESlot_Ammo) return "Ammo";
    if (slot == gESlot_Body) return "Body";
    if (slot == gESlot_Helmet) return "Helmet";
    switch (slot) {
    case 3: return "EquipLeft";
    case 4: return "Bow";
    case 5: return "Crossbow";
    case 8: return "Amulet";
    case 9: return "Ring1";
    case 10: return "Ring2";
    case 11: return "Belt";
    default: return "Slot";
    }
}

void CaptureHeroCarry(eCEntity* heroEntity, std::vector<CarryEntry>& out)
{
    out.clear();
    if (!heroEntity || !heroEntity->GetPropertySet(eEPropertySetType_Inventory)) return;
    Entity hero(heroEntity);
    const GEInt stackCount = hero.Inventory.GetStackCount();
    out.reserve(static_cast<size_t>(std::max<GEInt>(stackCount, 0)));
    for (GEInt i = 0; i < stackCount; ++i) {
        const int amt = hero.Inventory.GetAmount(i);
        if (amt <= 0) continue;
        Entity item = hero.Inventory.GetTemplateItem(i);
        char const* name = item.GetInstance() ? item.GetName().GetText() : nullptr;
        if (!name || !*name) continue;
        CarryEntry entry{};
        Format(entry.name, "%s", name);
        entry.amount = amt;
        out.push_back(entry);
    }
    std::sort(out.begin(), out.end(), [](CarryEntry const& a, CarryEntry const& b) {
        return std::strcmp(a.name, b.name) < 0;
    });
    size_t write = 0;
    for (size_t read = 0; read < out.size(); ++read) {
        if (write > 0 && std::strcmp(out[write - 1].name, out[read].name) == 0)
            out[write - 1].amount += out[read].amount;
        else
            out[write++] = out[read];
    }
    out.resize(write);
}

size_t FormatCarryDelta(std::vector<CarryEntry> const& before, std::vector<CarryEntry> const& after,
                        char* out, size_t outSize) noexcept
{
    size_t pos = 0, ib = 0, ia = 0;
    auto emit = [&](char const* name, int diff) {
        if (diff != 0 && pos + 64 < outSize)
            pos += static_cast<size_t>(Format(out + pos, outSize - pos, "%s%s:%+d", pos ? "," : "", name, diff));
    };
    while (ib < before.size() || ia < after.size()) {
        const int cmp = (ib >= before.size()) ? 1
                      : (ia >= after.size()) ? -1
                      : std::strcmp(before[ib].name, after[ia].name);
        if (cmp < 0) {
            emit(before[ib].name, -before[ib].amount);
            ++ib;
        } else if (cmp > 0) {
            emit(after[ia].name, after[ia].amount);
            ++ia;
        } else {
            emit(after[ia].name, after[ia].amount - before[ib].amount);
            ++ib;
            ++ia;
        }
    }
    return pos;
}

void CaptureHeroSlots(Entity& hero, SlotTable& out, int* outGold = nullptr) noexcept
{
    out.Clear();
    out.primed = true;
    int gold = 0;
    const GEInt count = hero.Inventory.GetStackCount();
    for (GEInt i = 0; i < count; ++i) {
        const int slot = static_cast<int>(hero.Inventory.GetSlotFromStack(i));
        if (slot == gESlot_None) {
            if (outGold) {
                Entity item = hero.Inventory.GetTemplateItem(i);
                if (item.GetInstance() && std::strcmp(item.GetName().GetText(), "It_Gold") == 0)
                    gold += hero.Inventory.GetAmount(i);
            }
            continue;
        }
        if (slot < gESlot_None || slot >= schema::MaxTrackedSlots) continue;
        Entity item = hero.Inventory.GetTemplateItem(i);
        char const* name = item.GetInstance() ? item.GetName().GetText() : "";
        out.Set(slot, name, npc_item_quality::MaskQuality(hero.Inventory.GetQuality(i)));
    }
    if (outGold) *outGold = gold;
}

bool CaptureHeroVitals(eCDynamicEntity* rawHero, Vitals& out, bool includeGold = true) noexcept
{
    if (!rawHero || !rawHero->GetPropertySet(eEPropertySetType_PlayerMemory) ||
        !rawHero->GetPropertySet(eEPropertySetType_NPC) ||
        !rawHero->GetPropertySet(eEPropertySetType_Inventory)) {
        out = Vitals{};
        return false;
    }
    Entity hero(rawHero);
    out.hp = hero.PlayerMemory.GetHitPoints();
    out.hpMax = hero.PlayerMemory.GetHitPointsMax();
    out.mp = hero.PlayerMemory.GetManaPoints();
    out.mpMax = hero.PlayerMemory.GetManaPointsMax();
    out.sp = hero.PlayerMemory.GetStaminaPoints();
    out.spMax = hero.PlayerMemory.GetMaximum(bCString("SP"));
    out.xp = static_cast<int>(hero.PlayerMemory.XP);
    out.level = static_cast<int>(hero.NPC.Level);
    out.lpAttribs = static_cast<int>(hero.PlayerMemory.LPAttribs);
    out.lpPerks = static_cast<int>(hero.PlayerMemory.LPPerks);
    out.gold = includeGold ? CountHeroGold(hero) : 0;
    out.chapter = static_cast<int>(hero.PlayerMemory.Chapter);
    if (auto* routine = static_cast<gCScriptRoutine_PS*>(rawHero->GetPropertySet(eEPropertySetType_ScriptRoutine))) {
        out.aiMode = static_cast<int>(routine->GetAIMode());
        out.aniState = static_cast<int>(routine->GetAniState());
    }
    out.valid = true;
    return true;
}

void EmitHeroBaseline(eCDynamicEntity* rawHero, Vitals const& v, SlotTable const& slots)
{
    Entity hero(rawHero);
    Event ev;
    const bCVector& p = rawHero->GetWorldPosition();
    ev.kv.Str("hero", SafeEntityName(rawHero)).Pos("pos", p.GetX(), p.GetY(), p.GetZ())
      .Int("hp", v.hp).Int("hp_max", v.hpMax).Int("mp", v.mp).Int("mp_max", v.mpMax)
      .Int("sp", v.sp).Int("sp_max", v.spMax).Int("level", v.level).Int("xp", v.xp)
      .Int("lp", v.lpAttribs).Int("gold", v.gold).Int("stacks", hero.Inventory.GetStackCount()).Int("chapter", v.chapter);
    char equipped[1024] = {};
    size_t pos = 0;
    for (int s = 1; s < schema::MaxTrackedSlots; ++s)
        if (slots.items[s][0])
            pos += static_cast<size_t>(Format(equipped + pos, sizeof(equipped) - pos, "%s%s:%s",
                                              pos ? "," : "", SlotLabel(s), slots.items[s]));
    ev.kv.Str("equipped", pos ? equipped : "none");
    ev.Send("hero.baseline");
}

void EmitEnclaveBaseline()
{
    char summary[3840] = {};
    size_t pos = 0;
    int notable = 0;
    const GEInt count = PSEnclave::GetEnclaveCount();
    for (GEInt i = 0; i < count; ++i) {
        Entity e = PSEnclave::GetEnclaveAt(i);
        eCEntity* inst = e.GetInstance();
        auto* enc = inst ? static_cast<gCEnclave_PS*>(inst->GetPropertySet(eEPropertySetType_Enclave)) : nullptr;
        if (!enc) continue;
        auto const& crime = enc->AccessCrimeCount();
        auto const& suspect = enc->AccessSuspectComment();
        const int theft = gEPlayerCrime_Theft < crime.GetCount() ? crime.GetAt(gEPlayerCrime_Theft) : 0;
        const int murder = gEPlayerCrime_Murder < crime.GetCount() ? crime.GetAt(gEPlayerCrime_Murder) : 0;
        const int attack = gEPlayerCrime_Attack < crime.GetCount() ? crime.GetAt(gEPlayerCrime_Attack) : 0;
        const int livestock = gEPlayerCrime_Livestock < crime.GetCount() ? crime.GetAt(gEPlayerCrime_Livestock) : 0;
        const int known = static_cast<int>(static_cast<gEPlayerCrime const&>(enc->AccessKnownPlayerCrime()));
        const int fame = enc->AccessPlayerFame();
        int suspectMask = 0;
        for (int c = 1; c < gEPlayerCrime_Count && c < suspect.GetCount(); ++c)
            if (suspect.GetAt(c)) suspectMask |= (1 << c);
        if (!theft && !murder && !attack && !livestock && known <= 0 && !suspectMask && !fame) continue;
        ++notable;
        pos += static_cast<size_t>(Format(summary + pos, sizeof(summary) - pos,
            "%s%s(fame:%d,theft:%d,murder:%d,atk:%d,live:%d,known:%s,susp:0x%x)",
            pos ? ";" : "", e.GetName().GetText(), fame, theft, murder, attack, livestock,
            CrimeLabel(known), suspectMask));
    }
    Event ev;
    ev.kv.Int("total_enclaves", count).Int("notable", notable).Str("enclaves", pos ? summary : "clean");
    ev.Send("enclave.baseline");
}

void EmitQuestBaseline()
{
    gCQuestManager_PS* qm = gCQuestManager_PS::GetInstance();
    if (!qm) return;
    auto& all = qm->AccessQuests();
    int running = 0, succeeded = 0, failed = 0, cancelled = 0;
    char activeList[3072] = {};
    size_t pos = 0;
    for (int i = 0; i < all.GetCount(); ++i) {
        auto* q = static_cast<gCQuest_PS*>(all.AccessAt(i));
        if (!q) continue;
        const int st = static_cast<int>(q->GetStatus().GetValue());
        if (st == gEQuestStatus_Running) {
            ++running;
            if (pos + 64 < sizeof(activeList))
                pos += static_cast<size_t>(Format(activeList + pos, sizeof(activeList) - pos, "%s%s",
                                                  pos ? "," : "", q->AccessName().GetText()));
        } else if (st == gEQuestStatus_Success || st == gEQuestStatus_Won) ++succeeded;
        else if (st == gEQuestStatus_Failed || st == gEQuestStatus_Lost) ++failed;
        else if (st == gEQuestStatus_Cancelled || st == gEQuestStatus_Obsolete) ++cancelled;
    }
    Event ev;
    ev.kv.Int("running", running).Int("succeeded", succeeded).Int("failed", failed).Int("cancelled", cancelled)
      .Str("running_quests", pos ? activeList : "none");
    ev.Send("quest.baseline");
}

void EmitWorldBaseline(eCDynamicEntity* rawHero, Vitals const& v, SlotTable const& slots)
{
    EmitHeroBaseline(rawHero, v, slots);
    EmitEnclaveBaseline();
    EmitQuestBaseline();
}

void ObserveHeroTransitions(eCDynamicEntity* rawHero, uint32_t nowMs)
{
    Vitals cur;
    if (!CaptureHeroVitals(rawHero, cur, false)) return;
    Entity hero(rawHero);
    SlotTable slots;
    CaptureHeroSlots(hero, slots, &cur.gold);
    const uint32_t epoch = net::WorldEpoch();
    if (!worldBaselineEmitted || baselineEpoch != epoch) {
        worldBaselineEmitted = true;
        baselineEpoch = epoch;
        prevVitals = cur;
        prevSlots = slots;
        EmitWorldBaseline(rawHero, cur, slots);
        return;
    }
    if (prevVitals.valid) {
        if (cur.hp != prevVitals.hp || cur.hpMax != prevVitals.hpMax) {
            Event ev;
            ev.kv.TransitionInt("hp", prevVitals.hp, cur.hp).Int("delta", cur.hp - prevVitals.hp)
              .TransitionInt("hp_max", prevVitals.hpMax, cur.hpMax)
              .Int("ai_mode", cur.aiMode).Int("ani_state", cur.aniState);
            lastCombatCause.AppendTo(ev.kv, nowMs, 2500, "combat_cause");
            ev.Send("hero.hp");
        }
        if (cur.level != prevVitals.level || cur.lpAttribs != prevVitals.lpAttribs ||
            cur.lpPerks != prevVitals.lpPerks || cur.chapter != prevVitals.chapter) {
            Event ev;
            ev.kv.TransitionInt("level", prevVitals.level, cur.level)
              .TransitionInt("lp", prevVitals.lpAttribs, cur.lpAttribs)
              .TransitionInt("lp_perks", prevVitals.lpPerks, cur.lpPerks)
              .TransitionInt("xp", prevVitals.xp, cur.xp)
              .TransitionInt("chapter", prevVitals.chapter, cur.chapter);
            lastQuestCause.AppendTo(ev.kv, nowMs, 5000, "quest_cause");
            ev.Send("hero.progression");
        }
        if (cur.gold != prevVitals.gold) {
            Event ev;
            ev.kv.TransitionInt("gold", prevVitals.gold, cur.gold).Int("delta", cur.gold - prevVitals.gold);
            lastLootCause.AppendTo(ev.kv, nowMs, 3000, "loot_cause");
            lastQuestCause.AppendTo(ev.kv, nowMs, 3000, "quest_cause");
            ev.Send("inventory.gold");
        }
    }
    slots.DiffFrom(prevSlots, [&](int slotIdx, char const* beforeItem, char const* afterItem,
                                  uint16_t qBefore, uint16_t qAfter) {
        Event ev;
        ev.kv.Str("who", "local_hero").Int("slot_id", slotIdx).Str("slot", SlotLabel(slotIdx))
          .TransitionStr("item", beforeItem, afterItem).TransitionInt("quality", qBefore, qAfter);
        ev.Send("equip.change");
    });
    prevVitals = cur;
    prevSlots = slots;
}

void ObserveLocalDialog()
{
    gCInfoManager_PS* infos = gCInfoManager_PS::GetInstance();
    const bool open = infos && infos->AccessIsRunning() != GEFalse;
    if (open && !wasDialogOpen) {
        eCEntity* partner = reinterpret_cast<eCEntity*>(infos->GetDialogPartnerNPC());
        wasDialogOpen = true;
        Format(dialogPartnerName, "%s", SafeEntityName(partner));
        if (partner) guid::FormatEntity(partner, dialogPartnerGuid, sizeof(dialogPartnerGuid));
        else Format(dialogPartnerGuid, "-");
        Event ev;
        AppendEntity(ev.kv, "partner", partner);
        if (partner && partner->GetPropertySet(eEPropertySetType_Dialog))
            ev.kv.Bool("talked_before", Entity(partner).Dialog.TalkedToPlayer != GEFalse);
        ev.Send("dialog.local_start");
    } else if (!open && wasDialogOpen) {
        wasDialogOpen = false;
        Event ev;
        ev.kv.Str("partner", dialogPartnerName).Str("partner_guid", dialogPartnerGuid);
        ev.Send("dialog.local_end");
    }
}

void ObserveRemotePeers()
{
    net::Remote const* remotes = net::Remotes();
    if (!remotes) return;
    for (uint8_t id = 1; id < net::MaxPlayers; ++id) {
        if (id == net::LocalId()) continue;
        net::Remote const& r = remotes[id];
        PeerSnap cur{};
        cur.connected = net::IsHost() ? net::PeerJoined(id) : r.connected;
        cur.inWorld = net::InWorld(id);
        cur.world = r.latest.world ? r.latest.world : r.activity.world;
        cur.downedSeq = r.latest.downedSequence ? r.latest.downedSequence : r.activity.downedSequence;
        cur.flags = r.latest.sequence ? r.latest.flags : r.activity.flags;
        cur.aniState = r.latest.aniState;
        Format(cur.name, "%s", r.latest.name[0] ? r.latest.name : net::PeerName(id));
        Format(cur.heldRight, "%s", r.latest.heldRight);
        Format(cur.heldLeft, "%s", r.latest.heldLeft);
        Format(cur.armor, "%s", r.latest.armor);
        Format(cur.helmet, "%s", r.latest.helmet);
        Format(cur.shape, "%s", r.latest.shape);
        std::memcpy(cur.pos, r.latest.position, sizeof(cur.pos));

        PeerSnap const& prev = prevPeers[id];
        if (cur.connected != prev.connected || cur.inWorld != prev.inWorld || cur.world != prev.world) {
            Event ev;
            ev.kv.Uint("peer", id).Str("name", cur.name)
              .TransitionInt("connected", prev.connected ? 1 : 0, cur.connected ? 1 : 0)
              .TransitionInt("in_world", prev.inWorld ? 1 : 0, cur.inWorld ? 1 : 0).Hex("world", cur.world)
              .Pos("pos", cur.pos[0], cur.pos[1], cur.pos[2]);
            ev.Send("peer.presence");
        }
        const uint8_t lifeMask = net::PlayerFlags::KnockedOut | net::PlayerFlags::Dead |
                                 net::PlayerFlags::Held | net::PlayerFlags::Robbable;
        if (cur.connected && ((cur.flags & lifeMask) != (prev.flags & lifeMask) || cur.downedSeq != prev.downedSeq)) {
            Event ev;
            ev.kv.Uint("peer", id).Str("name", cur.name)
              .Hex("flags_before", prev.flags).Hex("flags_after", cur.flags)
              .Bool("ko", (cur.flags & net::PlayerFlags::KnockedOut) != 0)
              .Bool("dead", (cur.flags & net::PlayerFlags::Dead) != 0)
              .Bool("held", (cur.flags & net::PlayerFlags::Held) != 0)
              .Bool("robbable", (cur.flags & net::PlayerFlags::Robbable) != 0)
              .TransitionInt("episode", prev.downedSeq, cur.downedSeq)
              .Pos("pos", cur.pos[0], cur.pos[1], cur.pos[2]);
            ev.Send("peer.life");
        }
        if (cur.connected && prev.connected && cur.inWorld && prev.inWorld && cur.EquipChangedFrom(prev)) {
            Event ev;
            ev.kv.Uint("peer", id).Str("name", cur.name)
              .TransitionStr("right", prev.heldRight, cur.heldRight)
              .TransitionStr("left", prev.heldLeft, cur.heldLeft)
              .TransitionStr("armor", prev.armor, cur.armor)
              .TransitionStr("helmet", prev.helmet, cur.helmet)
              .TransitionStr("shape", prev.shape, cur.shape);
            ev.Send("peer.equip");
        }
        if (cur.TeleportedFrom(prev)) {
            const float dx = cur.pos[0] - prev.pos[0], dy = cur.pos[1] - prev.pos[1], dz = cur.pos[2] - prev.pos[2];
            Event ev;
            ev.kv.Uint("peer", id).Str("name", cur.name)
              .Pos("from_pos", prev.pos[0], prev.pos[1], prev.pos[2])
              .Pos("to_pos", cur.pos[0], cur.pos[1], cur.pos[2])
              .Float1("dist_m", std::sqrt(dx * dx + dy * dy + dz * dz) * 0.01);
            ev.Send("peer.teleport");
        }
        prevPeers[id] = cur;
    }
}

void CaptureEntityGuid(eCEntity* entity, uint8_t out[16], bool& hasOut) noexcept
{
    if (!entity) return;
    const eCEntityProxy proxy(entity);
    std::memcpy(out, &proxy.GetEntityID().GetCore(), 16);
    hasOut = true;
}

} // namespace

void OnWorldActivate()
{
    worldBaselineEmitted = false;
    groundFilter.Reset();
    Event ev;
    ev.kv.Bool("connected", net::Connected()).Bool("joined", net::Joined());
    ev.Send("world.activate");
}

void OnWorldDeactivate()
{
    worldBaselineEmitted = false;
    wasDialogOpen = false;
    prevVitals = Vitals{};
    prevSlots.Clear();
    recentJudge = detail::CombatContext{};
    lastCombatCause.Clear();
    lastCrimeCause.Clear();
    lastLootCause.Clear();
    lastQuestCause.Clear();
    std::memset(prevPeers, 0, sizeof(prevPeers));
    groundFilter.Reset();
    Event{}.Send("world.deactivate");
}

void OnWorldChange()
{
    worldBaselineEmitted = false;
    wasDialogOpen = false;
    prevVitals = Vitals{};
    prevSlots.Clear();
    recentJudge = detail::CombatContext{};
    lastCombatCause.Clear();
    lastCrimeCause.Clear();
    lastLootCause.Clear();
    lastQuestCause.Clear();
    std::memset(prevPeers, 0, sizeof(prevPeers));
    groundFilter.Reset();
}

void OnEntityDestroyed(eCEntity const* entity)
{
    if (!entity) return;
    if (recentJudge.victim.GetEntity() == entity) recentJudge.victim = eCEntityProxy();
    if (recentJudge.attacker.GetEntity() == entity) recentJudge.attacker = eCEntityProxy();
    if (recentJudge.source.GetEntity() == entity) recentJudge.source = eCEntityProxy();
    if (activeTransfer) {
        if (activeTransfer->from_ == entity) activeTransfer->from_ = nullptr;
        if (activeTransfer->to_ == entity) activeTransfer->to_ = nullptr;
    }
}

void Tick()
{
    ObserveRemotePeers();
    if (!gCSession::GetInstance().IsGameRunning()) return;
    ObserveLocalDialog();
    if (eCDynamicEntity* hero = gCSession::GetInstance().GetHero()) ObserveHeroTransitions(hero, NowMs());
}

TransferScope::TransferScope(char const* mode, eCEntity* from, eCEntity* to, int stackIndex, int requestedAmount)
    : mode_(mode), from_(from), to_(to), stack_(stackIndex), requested_(requestedAmount)
{
    if (activeTransfer != nullptr) {
        nested_ = true;
        finished_ = true;
        return;
    }
    activeTransfer = this;
    Format(fromName_, "%s", SafeEntityName(from));
    Format(toName_, "%s", SafeEntityName(to));
    CaptureEntityGuid(from, fromGuid_, haveFromGuid_);
    CaptureEntityGuid(to, toGuid_, haveToGuid_);
    eCEntity* hero = gCSession::GetInstance().GetHero();
    involvesHero_ = hero && (from == hero || to == hero);
    if (from && stackIndex >= 0 && from->GetPropertySet(eEPropertySetType_Inventory)) {
        Entity src(from);
        if (stackIndex < src.Inventory.GetStackCount()) {
            Entity tpl = src.Inventory.GetTemplateItem(stackIndex);
            Format(stackItem_, "%s", tpl.GetInstance() ? tpl.GetName().GetText() : "-");
            stackAmountBefore_ = src.Inventory.GetAmount(stackIndex);
        }
    }
    if (involvesHero_ && hero->GetPropertySet(eEPropertySetType_Inventory)) {
        Entity h(hero);
        goldBefore_ = CountHeroGold(h);
        CaptureHeroCarry(hero, activeHeroCarryBefore);
    }
}

TransferScope::~TransferScope()
{
    if (!nested_ && activeTransfer == this) {
        activeTransfer = nullptr;
        activeHeroCarryBefore.clear();
        activeHeroCarryAfter.clear();
    }
}

bool TransferScope::Finish(bool ok)
{
    if (nested_ || finished_) return ok;
    finished_ = true;
    eCEntity* hero = gCSession::GetInstance().GetHero();
    int goldAfter = goldBefore_;
    char deltaSummary[1536] = {};
    size_t pos = 0;
    if (involvesHero_ && hero && hero->GetPropertySet(eEPropertySetType_Inventory)) {
        Entity h(hero);
        goldAfter = CountHeroGold(h);
        CaptureHeroCarry(hero, activeHeroCarryAfter);
        pos = FormatCarryDelta(activeHeroCarryBefore, activeHeroCarryAfter, deltaSummary, sizeof(deltaSummary));
    }
    if (!involvesHero_ && !ok) return ok;
    Event ev;
    ev.kv.Str("mode", mode_ ? mode_ : "transfer").Bool("ok", ok);
    AppendEntity(ev.kv, "from", from_, haveFromGuid_ ? fromGuid_ : nullptr, fromName_);
    AppendEntity(ev.kv, "to", to_, haveToGuid_ ? toGuid_ : nullptr, toName_);
    if (stack_ >= 0)
        ev.kv.Int("stack", stack_).Str("item", stackItem_[0] ? stackItem_ : "-").Int("stack_before", stackAmountBefore_);
    if (requested_ >= 0) ev.kv.Int("requested", requested_);
    if (involvesHero_)
        ev.kv.Int("gold_delta", goldAfter - goldBefore_).Str("hero_deltas", pos ? deltaSummary : "none");
    const uint64_t seq = ev.Send("loot.transfer");
    if (ok && involvesHero_)
        lastLootCause.Record(seq, NowMs(), mode_ ? mode_ : "loot", fromName_, toName_, pos ? deltaSummary : stackItem_);
    return ok;
}

} // namespace g3t::telemetry
