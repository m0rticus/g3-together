// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote_internal.h"

#include "guards.h"
#include "log.h"
#include "npc_item_quality_policy.h"
#include "persist.h"

#include <cstring>

namespace g3t::remote::internal {
namespace {

static_assert(player_equip::Slots[player_equip::SlotRightHand].nativeSlot == gESlot_RightHand, "RightHand slot mismatch");
static_assert(player_equip::Slots[player_equip::SlotLeftHand].nativeSlot == gESlot_LeftHand, "LeftHand slot mismatch");
static_assert(player_equip::Slots[player_equip::SlotEquipLeftHand].nativeSlot == gESlot_EquipLeftHand, "EquipLeftHand slot mismatch");
static_assert(player_equip::Slots[player_equip::SlotBackLeft].nativeSlot == gESlot_BackLeft, "BackLeft slot mismatch");
static_assert(player_equip::Slots[player_equip::SlotBackRight].nativeSlot == gESlot_BackRight, "BackRight slot mismatch");
static_assert(player_equip::Slots[player_equip::SlotBow].nativeSlot == gESlot_Bow, "Bow slot mismatch");
static_assert(player_equip::Slots[player_equip::SlotCrossbow].nativeSlot == gESlot_Crossbow, "Crossbow slot mismatch");
static_assert(player_equip::Slots[player_equip::SlotArmor].nativeSlot == gESlot_Armor, "Armor slot mismatch");
static_assert(player_equip::Slots[player_equip::SlotBelt].nativeSlot == gESlot_Belt, "Belt slot mismatch");
static_assert(player_equip::Slots[player_equip::SlotAmmo].nativeSlot == gESlot_Ammo, "Ammo slot mismatch");
static_assert(player_equip::Slots[player_equip::SlotBody].nativeSlot == gESlot_Body, "Body slot mismatch");
static_assert(player_equip::Slots[player_equip::SlotHelmet].nativeSlot == gESlot_Helmet, "Helmet slot mismatch");

struct PuppetInventoryOps {
    Entity body;

    player_equip::SlotItem ReadSlot(uint8_t nativeSlot) const
    {
        player_equip::SlotItem item{};
        if (!body.GetInstance() || !body.GetInstance()->GetPropertySet(eEPropertySetType_Inventory))
            return item;
        const gESlot slot = static_cast<gESlot>(nativeSlot);
        const Entity attached = body.Inventory.GetItemFromSlot(slot);
        if (!attached.GetInstance()) return item;
        player_equip::CopyItemName(item.name, attached.GetName().GetText());
        const GEInt idx = body.Inventory.FindStackIndex(slot);
        item.quality = idx >= 0 ? npc_item_quality::MaskQuality(body.Inventory.GetQuality(idx)) : 0u;
        return item;
    }

    void UnlinkSlot(uint8_t nativeSlot)
    {
        if (!body.GetInstance()) return;
        TakeOff(body, static_cast<gESlot>(nativeSlot));
    }

    int AcquireUnlinkedStack(char const* name, uint16_t quality, uint8_t nativeSlot)
    {
        if (!name || !*name || !body.GetInstance()) return -1;
        const Template templ(name);
        if (!templ.IsValid()) return -1;
        const GEInt count = body.Inventory.GetStackCount();
        for (GEInt i = 0; i < count; ++i) {
            if (body.Inventory.GetSlotFromStack(i) != gESlot_None) continue;
            if (body.Inventory.GetAmount(i) < 1) continue;
            Entity item = body.Inventory.GetTemplateItem(i);
            if (!item.GetInstance() || std::strcmp(item.GetName().GetText(), name) != 0) continue;
            if (npc_item_quality::MaskQuality(body.Inventory.GetQuality(i)) != quality) continue;
            if (body.Inventory.GetAmount(i) > 1 && nativeSlot != gESlot_Ammo) {
                const GEInt split = body.Inventory.SplitStack(i, 1);
                if (split >= 0) return split;
            }
            return i;
        }
        const GEInt created = body.Inventory.CreateItems(templ, quality, 1);
        if (created < 0) return -1;
        if (body.Inventory.GetSlotFromStack(created) != gESlot_None ||
            (body.Inventory.GetAmount(created) > 1 && nativeSlot != gESlot_Ammo)) {
            const GEInt split = body.Inventory.SplitStack(created, 1);
            if (split >= 0) return split;
        }
        return body.Inventory.GetSlotFromStack(created) == gESlot_None ? created : -1;
    }

    bool LinkToSlot(int stackIndex, uint8_t nativeSlot)
    {
        if (stackIndex < 0 || !body.GetInstance()) return false;
        const gESlot slot = static_cast<gESlot>(nativeSlot);
        if (body.Inventory.EquipStackToSlot(stackIndex, slot) &&
            body.Inventory.GetSlotFromStack(stackIndex) == slot)
            return true;
        auto* inv = static_cast<gCInventory_PS*>(body.GetInstance()->GetPropertySet(eEPropertySetType_Inventory));
        return inv && inv->LinkStackToSlot(stackIndex, slot, GETrue) &&
               body.Inventory.GetSlotFromStack(stackIndex) == slot;
    }

    void DisableHandCollision(uint8_t nativeSlot)
    {
        if (!body.GetInstance()) return;
        const Entity held = body.Inventory.GetItemFromSlot(static_cast<gESlot>(nativeSlot));
        if (held.GetInstance() && held.GetInstance()->HasCollision())
            held.GetInstance()->EnableCollision(GEFalse);
    }
};

} // namespace

bool CaptureLocalEquipment(player_equip::PlayerEquipment& out)
{
    gCSession& session = gCSession::GetInstance();
    eCDynamicEntity* own = session.GetHero();
    out = player_equip::PlayerEquipment{};
    out.world = CurrentWorld();
    out.epoch = net::WorldEpoch();
    if (!own || !session.IsGameRunning() || persist::HeroPending()) return false;
    eCEntity* controlled = ControlledBody();
    out.transformed = controlled && controlled != own;
    if (out.transformed || qa.badStates) return true;
    if (!own->GetPropertySet(eEPropertySetType_Inventory)) return true;

    Entity mine(own);
    for (size_t i = 0; i < player_equip::SlotCount; ++i) {
        const gESlot slot = static_cast<gESlot>(player_equip::Slots[i].nativeSlot);
        const Entity item = mine.Inventory.GetItemFromSlot(slot);
        if (!item.GetInstance()) continue;
        const GEInt idx = mine.Inventory.FindStackIndex(slot);
        const uint16_t quality = idx >= 0 ? npc_item_quality::MaskQuality(mine.Inventory.GetQuality(idx)) : 0u;
        out.SetSlot(i, item.GetName().GetText(), quality);
    }
    return true;
}

void UnlinkMismatchedEquipment(Puppet& puppet, player_equip::PlayerEquipment const& wanted,
                               bool applyWeapons, bool applyWear)
{
    eCEntity* entity = puppet.entity.GetInstance();
    if (!entity || guards::RejectRemoteWrite(entity, "remote_equip::Unlink")) return;
    PuppetInventoryOps ops{puppet.entity};
    player_equip::UnlinkMismatchedSlots(ops, wanted, applyWeapons, applyWear);
}

void ApplyMissingEquipment(Puppet& puppet, player_equip::PlayerEquipment const& wanted,
                           bool applyWeapons, bool applyWear)
{
    eCEntity* entity = puppet.entity.GetInstance();
    if (!entity || guards::RejectRemoteWrite(entity, "remote_equip::Apply")) return;
    PuppetInventoryOps ops{puppet.entity};
    player_equip::ApplyMissingSlots(ops, wanted, applyWeapons, applyWear, puppet.failedEquip);
    const Entity finalRight = puppet.entity.Inventory.GetItemFromSlot(gESlot_RightHand);
    const Entity finalLeft = puppet.entity.Inventory.GetItemFromSlot(gESlot_LeftHand);
    puppet.heldRight = finalRight.GetInstance() ? finalRight.GetName().GetText() : "";
    puppet.heldLeft = finalLeft.GetInstance() ? finalLeft.GetName().GetText() : "";
}

void SyncAllEquipment(Puppet& puppet, player_equip::PlayerEquipment const& wanted,
                      bool applyWeapons, bool applyWear)
{
    eCEntity* entity = puppet.entity.GetInstance();
    if (!entity || guards::RejectRemoteWrite(entity, "remote_equip::Sync")) return;
    PuppetInventoryOps ops{puppet.entity};
    player_equip::SyncSlots(ops, wanted, applyWeapons, applyWear, puppet.failedEquip);
    const Entity finalRight = puppet.entity.Inventory.GetItemFromSlot(gESlot_RightHand);
    const Entity finalLeft = puppet.entity.Inventory.GetItemFromSlot(gESlot_LeftHand);
    puppet.heldRight = finalRight.GetInstance() ? finalRight.GetName().GetText() : "";
    puppet.heldLeft = finalLeft.GetInstance() ? finalLeft.GetName().GetText() : "";
}

} // namespace g3t::remote::internal
