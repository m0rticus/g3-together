// SPDX-License-Identifier: GPL-3.0-or-later
// Remote puppet hand-ammo linking, hero upper-body/weapon overlay capture, and puppet overlay playback.
#include "ranged.h"

#include "guid_key.h"
#include "movement_policy.h"
#include "npcplayback_hooks.h"
#include "ranged_anim.h"
#include "remote.h"

#include <Game.h>
#include <Script.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace g3t::ranged {
namespace {
struct HeroSlotTracker {
    uintptr_t instance = 0;
    float lastTime = 0.0f;
    char name[96] = {};
    uint8_t seq = 0;
};

struct HeroOverlayTracker {
    eCEntity* hero = nullptr;
    HeroSlotTracker slots[8]{};
    eCEntity* weapon = nullptr;
    HeroSlotTracker weaponSlot{};
} heroTracker;

struct DrivenSlotState {
    uint32_t metaBits = 0;
    char file[128] = {};
};

struct PuppetOverlayState {
    eCEntityProxy entityProxy;
    uint8_t drivenMask = 0;
    DrivenSlotState slots[8]{};
    eCEntity* weapon = nullptr;
    eCEntityProxy weaponProxy;
    uint8_t weaponSeq = 0;
    bool weaponDriven = false;
    char weaponFile[128] = {};
};

struct PropertyIdHash {
    size_t operator()(bCPropertyID const& id) const noexcept
    {
        GuidKey key{};
        std::memcpy(key.data(), &id.GetCore(), 16);
        return GuidHash{}(key);
    }
};

std::unordered_map<bCPropertyID, PuppetOverlayState, PropertyIdHash> puppetOverlays;

uintptr_t RawMotionInstance(eCWrapper_emfx2Actor const& actor, int slot)
{
    uintptr_t instance = 0;
    std::memcpy(&instance, reinterpret_cast<unsigned char const*>(&actor) + 0x10 + slot * sizeof(void*), sizeof(instance));
    return instance;
}

float RawSlotWeight(eCWrapper_emfx2Actor const& actor, int slot)
{
    unsigned char const* instance = nullptr;
    std::memcpy(&instance, reinterpret_cast<unsigned char const*>(&actor) + 0x10 + slot * sizeof(void*), sizeof(instance));
    float weight = 0.0f;
    if (instance) std::memcpy(&weight, instance + 0x24, sizeof(weight));
    return std::isfinite(weight) ? std::clamp(weight, 0.0f, 1.0f) : 0.0f;
}

uint8_t AdvanceRestartSeq(HeroSlotTracker& tracker, uintptr_t instance, char const* name, float playTime, bool looping)
{
    const bool sameName = std::strcmp(tracker.name, name) == 0;
    if (tracker.instance != 0 && sameName && !looping &&
        (instance != tracker.instance || playTime + 0.03f < tracker.lastTime)) {
        tracker.seq = static_cast<uint8_t>((tracker.seq + 1u) & 0x3u);
    } else if (!sameName) {
        strncpy_s(tracker.name, name, _TRUNCATE);
    }
    tracker.instance = instance;
    tracker.lastTime = playTime;
    return tracker.seq;
}

void NormalizeXmotFile(char const* raw, char (&out)[128])
{
    if (!raw || !*raw) {
        out[0] = '\0';
        return;
    }
    if (std::strstr(raw, ".xmot")) strncpy_s(out, raw, _TRUNCATE);
    else std::snprintf(out, sizeof(out), "%s.xmot", raw);
}

GEInt EnsurePuppetStack(Entity& body, char const* name)
{
    if (!name || !*name) return -1;
    for (int pass = 0; pass < 2; ++pass) {
        const GEInt count = body.Inventory.GetStackCount();
        for (GEInt i = 0; i < count; ++i) {
            Entity item = body.Inventory.GetTemplateItem(i);
            if (item.GetInstance() && !std::strcmp(item.GetName().GetText(), name)) {
                if (body.Inventory.GetAmount(i) < 2)
                    body.Inventory.CreateItems(Template(name), 0, 2);
                return i;
            }
        }
        if (pass == 0) body.Inventory.CreateItems(Template(name), 0, 2);
    }
    return -1;
}

void UnlinkPuppetSlot(Entity& body, gESlot slot)
{
    for (GEInt i = 0; i < body.Inventory.GetStackCount(); ++i)
        if (body.Inventory.GetSlotFromStack(i) == slot) body.Inventory.UnEquipStack(i);
    if (auto* inv = static_cast<gCInventory_PS*>(body.GetInstance()->GetPropertySet(eEPropertySetType_Inventory))) {
        if (body.Inventory.FindStackIndex(slot) >= 0) inv->UnlinkStackFromSlot(slot);
    }
}

using MotionOwner = eCVisualAnimation_PS::eSMotionDesc::eEMotionOwner;

struct MotionPlayDesc {
    float blendInTime;
    int32_t playMode;
    float playSpeed;
    int32_t maxLoops;
    float targetWeight;
    float blendOutTime;
    int32_t blendMode;
};
static_assert(sizeof(MotionPlayDesc) == 28);

void StopAnimationSlot(eCVisualAnimation_PS* animation, eCWrapper_emfx2Actor* actor,
                       eCWrapper_emfx2Actor::eEMotionType slot, float blendOut)
{
    if (!animation || !actor) return;
    animation->SetMotionOwner(slot, static_cast<MotionOwner>(2));
    actor->SetPlaySpeed(slot, 1.0f);
    if (actor->HasMotionInstance(slot)) animation->StopMotion(slot, blendOut);
}

void StopDrivenWeapon(PuppetOverlayState& state)
{
    if (!state.weaponDriven || !state.weapon) {
        state.weapon = nullptr;
        state.weaponProxy = eCEntityProxy();
        state.weaponDriven = false;
        state.weaponFile[0] = '\0';
        return;
    }
    if (eCEntity* live = state.weaponProxy.GetEntity(); live && live == state.weapon) {
        auto* anim = static_cast<eCVisualAnimation_PS*>(live->GetPropertySet(eEPropertySetType_Animation));
        auto* actor = anim ? anim->GetActor() : nullptr;
        StopAnimationSlot(anim, actor, static_cast<eCWrapper_emfx2Actor::eEMotionType>(0), 0.05f);
    }
    state.weapon = nullptr;
    state.weaponProxy = eCEntityProxy();
    state.weaponDriven = false;
    state.weaponFile[0] = '\0';
}

void DriveFrozenMotion(eCWrapper_emfx2Actor* actor, eCWrapper_emfx2Actor::eEMotionType slot,
                       float phase, float weight)
{
    const float duration = static_cast<float>(actor->GetMaxTime(slot));
    if (duration > 0.0f && std::isfinite(duration)) {
        actor->SetIntroExtroBlending(slot, 0.0f, 0.0f);
        actor->SetTargetWeight(slot, weight);
        actor->SetPlaySpeed(slot, 0.0f);
        actor->PauseMotion(GEFalse, slot);
        actor->SetPlayTime(slot, std::min(duration * phase, std::nextafter(duration, 0.0f)));
    }
}

void PresentOverlayChannels(eCVisualAnimation_PS* animation, eCWrapper_emfx2Actor* actor,
                            PuppetOverlayState& state, ranged_anim::OverlayMeta const& meta,
                            uint32_t overlayMeta, char const* anim0, char const* anim1,
                            ranged_anim::OverlayHot const& hot)
{
    struct DesiredSlot {
        bool active = false;
        ranged_anim::OverlayChannelMeta ch{};
        uint32_t rawBits = 0;
        float phase = 0.0f;
        float weight = 0.0f;
        char file[128] = {};
    } desired[8]{};

    auto stageChannel = [&](ranged_anim::OverlayChannelMeta const& ch, uint32_t rawBits,
                            char const* rawName, uint8_t qPhase, uint8_t qWeight) {
        if (ch.slot < 1 || ch.slot > 7 || !rawName || !*rawName) return;
        DesiredSlot& d = desired[ch.slot];
        NormalizeXmotFile(rawName, d.file);
        if (!d.file[0]) return;
        d.active = true;
        d.ch = ch;
        d.rawBits = rawBits;
        d.phase = ranged_anim::DequantizeUnit(qPhase);
        d.weight = std::max(ranged_anim::DequantizeUnit(qWeight), 0.001f);
    };
    stageChannel(meta.ch0, overlayMeta & 0xfffu, anim0, hot.phase0, hot.weight0);
    stageChannel(meta.ch1, (overlayMeta >> 12) & 0xfffu, anim1, hot.phase1, hot.weight1);

    uint8_t nextMask = 0;
    for (int s = 1; s <= 7; ++s) {
        const auto slot = static_cast<eCWrapper_emfx2Actor::eEMotionType>(s);
        const uint8_t bit = static_cast<uint8_t>(1u << s);
        DesiredSlot const& d = desired[s];
        if (!d.active) {
            if (state.drivenMask & bit) {
                StopAnimationSlot(animation, actor, slot, 0.08f);
                state.slots[s] = DrivenSlotState{};
            }
            continue;
        }
        const bool restart = (state.drivenMask & bit) == 0 ||
                             state.slots[s].metaBits != d.rawBits ||
                             std::strcmp(state.slots[s].file, d.file) != 0 ||
                             !actor->HasMotionInstance(slot);
        if (restart) {
            auto& desc = const_cast<eCVisualAnimation_PS::eSMotionDesc&>(animation->GetMotionDesc(slot));
            desc.SetMotion(bCString(d.file));
            auto* resource = desc.GetMotion();
            if (!resource) continue;
            animation->SetMotion(slot, resource);
            MotionPlayDesc play{0.0f, static_cast<int32_t>(d.ch.playMode), 1.0f, d.ch.looping ? -1 : 1,
                                d.weight, 0.0f, static_cast<int32_t>(d.ch.blendMode)};
            animation->PlayMotion(slot, d.ch.defaults
                ? nullptr
                : reinterpret_cast<eCWrapper_emfx2Motion::eSMotionDesc*>(&play));
            animation->SetMotionOwner(slot, static_cast<MotionOwner>(4));
            state.slots[s].metaBits = d.rawBits;
            strncpy_s(state.slots[s].file, d.file, _TRUNCATE);
        }
        if (!actor->HasMotionInstance(slot)) continue;
        actor->SetBlendMode(slot, static_cast<eCWrapper_emfx2Motion::eEMotionBlendMode>(d.ch.blendMode));
        DriveFrozenMotion(actor, slot, d.phase, d.weight);
        nextMask |= bit;
    }
    state.drivenMask = nextMask;
}

void PresentWeaponMotion(eCEntity* puppetEntity, PuppetOverlayState& state,
                         ranged_anim::OverlayMeta const& meta, char const* weaponAnim,
                         ranged_anim::OverlayHot const& hot)
{
    char wantedWeaponFile[128] = {};
    if (meta.weaponActive && weaponAnim && *weaponAnim)
        NormalizeXmotFile(weaponAnim, wantedWeaponFile);
    eCEntity* heldLeft = puppetEntity->GetPropertySet(eEPropertySetType_Inventory)
        ? Entity(puppetEntity).Inventory.GetItemFromSlot(gESlot_LeftHand).GetInstance()
        : nullptr;
    if (!wantedWeaponFile[0] || !heldLeft) {
        StopDrivenWeapon(state);
        return;
    }
    if (state.weapon && state.weapon != heldLeft)
        StopDrivenWeapon(state);
    auto* wAnim = static_cast<eCVisualAnimation_PS*>(heldLeft->GetPropertySet(eEPropertySetType_Animation));
    auto* wActor = wAnim ? wAnim->GetActor() : nullptr;
    const auto slot0 = static_cast<eCWrapper_emfx2Actor::eEMotionType>(0);
    if (!wAnim || !wActor) return;
    const bool wRestart = !state.weaponDriven || state.weapon != heldLeft ||
                          state.weaponSeq != meta.weaponSeq ||
                          std::strcmp(state.weaponFile, wantedWeaponFile) != 0 ||
                          !wActor->HasMotionInstance(slot0);
    if (wRestart) {
        auto& wDesc = const_cast<eCVisualAnimation_PS::eSMotionDesc&>(wAnim->GetMotionDesc(slot0));
        wDesc.SetMotion(bCString(wantedWeaponFile));
        if (auto* wRes = wDesc.GetMotion()) {
            MotionPlayDesc wPlay{0.0f, 0, 1.0f, 1, 1.0f, 0.0f, 1};
            wAnim->SetMotion(slot0, wRes);
            wAnim->PlayMotion(slot0, reinterpret_cast<eCWrapper_emfx2Motion::eSMotionDesc*>(&wPlay));
            wAnim->SetMotionOwner(slot0, static_cast<MotionOwner>(4));
            state.weapon = heldLeft;
            state.weaponProxy = eCEntityProxy(heldLeft);
            state.weaponSeq = meta.weaponSeq;
            state.weaponDriven = true;
            strncpy_s(state.weaponFile, wantedWeaponFile, _TRUNCATE);
        }
    }
    if (state.weaponDriven && wActor->HasMotionInstance(slot0))
        DriveFrozenMotion(wActor, slot0, ranged_anim::DequantizeUnit(hot.weaponPhase), 1.0f);
}
} // namespace

void SyncPuppetHands(eCEntity* puppetEntity, char const* right, char const* left,
                     std::string& heldRightOut, std::string& heldLeftOut)
{
    if (!puppetEntity) return;
    Entity body(puppetEntity);
    char const* wantRight = right ? right : "";
    char const* wantLeft = left ? left : "";
    if (!*wantLeft) UnlinkPuppetSlot(body, gESlot_LeftHand);
    if (!*wantRight) UnlinkPuppetSlot(body, gESlot_RightHand);
    GEInt leftStack = EnsurePuppetStack(body, wantLeft);
    const GEInt rightStack = EnsurePuppetStack(body, wantRight);
    if (*wantLeft) leftStack = EnsurePuppetStack(body, wantLeft);
    if (leftStack >= 0 || rightStack >= 0)
        body.Inventory.HoldStacks(leftStack, rightStack);

    auto slotMatches = [&](gESlot slot, char const* expected) {
        const Entity inSlot = body.Inventory.GetItemFromSlot(slot);
        char const* have = inSlot.GetInstance() ? inSlot.GetName().GetText() : "";
        return std::strcmp(have, expected) == 0;
    };
    auto* inv = static_cast<gCInventory_PS*>(puppetEntity->GetPropertySet(eEPropertySetType_Inventory));
    if (*wantLeft && !slotMatches(gESlot_LeftHand, wantLeft)) {
        const GEInt idx = leftStack;
        if (idx >= 0) {
            body.Inventory.HoldLeftStack(idx);
            if (!slotMatches(gESlot_LeftHand, wantLeft) && inv)
                inv->LinkStackToSlot(idx, gESlot_LeftHand, GETrue);
        }
    }
    // HoldStacks skips linking ammo (Arrow/Bolt) into RightHand when a Bow/CrossBow occupies LeftHand;
    // PS_Ranged_Reload links ammo explicitly, which switches _Bow_None_ motions to _Bow_Arrow_.
    if (*wantRight && !slotMatches(gESlot_RightHand, wantRight)) {
        const GEInt idx = rightStack;
        if (idx >= 0) {
            body.Inventory.HoldRightStack(idx);
            if (!slotMatches(gESlot_RightHand, wantRight) && inv)
                inv->LinkStackToSlot(idx, gESlot_RightHand, GETrue);
        }
    }
    const Entity finalRight = body.Inventory.GetItemFromSlot(gESlot_RightHand);
    const Entity finalLeft = body.Inventory.GetItemFromSlot(gESlot_LeftHand);
    if (finalRight.GetInstance() && finalRight.GetInstance()->HasCollision()) finalRight.GetInstance()->EnableCollision(GEFalse);
    if (finalLeft.GetInstance() && finalLeft.GetInstance()->HasCollision()) finalLeft.GetInstance()->EnableCollision(GEFalse);
    heldRightOut = finalRight.GetInstance() ? finalRight.GetName().GetText() : "";
    heldLeftOut = finalLeft.GetInstance() ? finalLeft.GetName().GetText() : "";
}

void CaptureHeroOverlay(eCEntity* hero, net::PlayerState& state)
{
    state.overlayAnim0[0] = '\0';
    state.overlayAnim1[0] = '\0';
    state.weaponAnim[0] = '\0';
    state.overlayMeta = 0;
    if (!hero || (state.flags & (net::PlayerFlags::KnockedOut | net::PlayerFlags::Dead | net::PlayerFlags::Held)) != 0)
        return;
    if (heroTracker.hero != hero) {
        heroTracker = HeroOverlayTracker{};
        heroTracker.hero = hero;
    }
    auto* animation = static_cast<eCVisualAnimation_PS*>(hero->GetPropertySet(eEPropertySetType_Animation));
    auto* actor = animation ? animation->GetActor() : nullptr;
    if (!actor) return;

    struct Candidate {
        int slot = 0;
        float weight = 0.0f;
        float phase = 0.0f;
        ranged_anim::OverlayChannelMeta meta{};
        char name[96] = {};
    };
    Candidate candidates[7]{};
    int candidateCount = 0;
    for (int s = 1; s <= 7; ++s) {
        const auto slot = static_cast<eCWrapper_emfx2Actor::eEMotionType>(s);
        if (!animation->HasMotion(slot) || !actor->HasMotionInstance(slot)) {
            heroTracker.slots[s].instance = 0;
            continue;
        }
        const float curWeight = RawSlotWeight(*actor, s);
        const float tgtWeight = std::isfinite(actor->GetTargetWeight(slot))
            ? std::clamp(actor->GetTargetWeight(slot), 0.0f, 1.0f) : 0.0f;
        const float effectiveWeight = curWeight > 0.0001f ? curWeight : tgtWeight;
        if (effectiveWeight <= 0.0001f) {
            heroTracker.slots[s].instance = 0;
            continue;
        }
        auto& desc = const_cast<eCVisualAnimation_PS::eSMotionDesc&>(animation->GetMotionDesc(slot));
        if (!desc.GetMotion()) continue;
        char const* rawName = desc.GetMotionFilename().GetText();
        if (!rawName || !*rawName || std::strlen(rawName) >= sizeof(state.overlayAnim0)) continue;
        const float maxTime = static_cast<float>(actor->GetMaxTime(slot));
        if (!(maxTime > 0.0f) || !std::isfinite(maxTime)) continue;
        const float playTime = std::clamp(static_cast<float>(actor->GetPlayTime(slot)), 0.0f, maxTime);

        Candidate& c = candidates[candidateCount++];
        c.slot = s;
        c.weight = effectiveWeight;
        c.phase = std::clamp(playTime / maxTime, 0.0f, 1.0f);
        strncpy_s(c.name, rawName, _TRUNCATE);

        c.meta.slot = static_cast<uint8_t>(s);
        c.meta.blendMode = static_cast<uint8_t>(static_cast<int>(actor->GetBlendMode(slot)) & 0x7);
        c.meta.playMode = 0;
        c.meta.looping = movement::IsLoopingPuppetMotion(rawName);
        c.meta.defaults = false;
        npcplayback::detail::CapturedOptions captured;
        if (npcplayback::detail::LookupCapturedOptions(actor, s, captured) && captured.available) {
            c.meta.defaults = captured.defaults;
            if (!captured.defaults) {
                int32_t rawPlayMode = 0, maxLoops = 1;
                std::memcpy(&rawPlayMode, &captured.words[1], sizeof(rawPlayMode));
                std::memcpy(&maxLoops, &captured.words[3], sizeof(maxLoops));
                c.meta.playMode = static_cast<uint8_t>(rawPlayMode & 0x3);
                if (maxLoops < 0) c.meta.looping = true;
            }
        }
        c.meta.seq = AdvanceRestartSeq(heroTracker.slots[s], RawMotionInstance(*actor, s), rawName, playTime, c.meta.looping);
    }

    if (candidateCount > 2) {
        std::sort(candidates, candidates + candidateCount, [](Candidate const& a, Candidate const& b) {
            return a.weight != b.weight ? a.weight > b.weight : a.slot < b.slot;
        });
        candidateCount = 2;
        if (candidates[0].slot > candidates[1].slot) std::swap(candidates[0], candidates[1]);
    }

    ranged_anim::OverlayMeta meta{};
    ranged_anim::OverlayHot hot{};
    if (candidateCount >= 1) {
        meta.ch0 = candidates[0].meta;
        strncpy_s(state.overlayAnim0, candidates[0].name, _TRUNCATE);
        hot.phase0 = ranged_anim::QuantizeUnit(candidates[0].phase);
        hot.weight0 = ranged_anim::QuantizeUnit(candidates[0].weight);
    }
    if (candidateCount >= 2) {
        meta.ch1 = candidates[1].meta;
        strncpy_s(state.overlayAnim1, candidates[1].name, _TRUNCATE);
        hot.phase1 = ranged_anim::QuantizeUnit(candidates[1].phase);
        hot.weight1 = ranged_anim::QuantizeUnit(candidates[1].weight);
    }

    if (hero->GetPropertySet(eEPropertySetType_Inventory)) {
        const Entity leftItem = Entity(hero).Inventory.GetItemFromSlot(gESlot_LeftHand);
        eCEntity* weapon = leftItem.GetInstance();
        if (heroTracker.weapon != weapon) {
            heroTracker.weapon = weapon;
            heroTracker.weaponSlot = HeroSlotTracker{};
        }
        auto* wAnim = weapon ? static_cast<eCVisualAnimation_PS*>(weapon->GetPropertySet(eEPropertySetType_Animation)) : nullptr;
        auto* wActor = wAnim ? wAnim->GetActor() : nullptr;
        const auto slot0 = static_cast<eCWrapper_emfx2Actor::eEMotionType>(0);
        if (wAnim && wActor && wAnim->HasMotion(slot0) && wActor->HasMotionInstance(slot0)) {
            const float wWeight = RawSlotWeight(*wActor, 0);
            auto& wDesc = const_cast<eCVisualAnimation_PS::eSMotionDesc&>(wAnim->GetMotionDesc(slot0));
            char const* wName = wDesc.GetMotion() ? wDesc.GetMotionFilename().GetText() : nullptr;
            const float wMax = static_cast<float>(wActor->GetMaxTime(slot0));
            if ((wWeight > 0.0001f || wActor->GetTargetWeight(slot0) > 0.0001f) &&
                wName && *wName && std::strlen(wName) < sizeof(state.weaponAnim) &&
                wMax > 0.0f && std::isfinite(wMax)) {
                const float wTime = std::clamp(static_cast<float>(wActor->GetPlayTime(slot0)), 0.0f, wMax);
                meta.weaponActive = true;
                meta.weaponSeq = AdvanceRestartSeq(heroTracker.weaponSlot, RawMotionInstance(*wActor, 0), wName, wTime, false);
                strncpy_s(state.weaponAnim, wName, _TRUNCATE);
                hot.weaponPhase = ranged_anim::QuantizeUnit(wTime / wMax);
            } else {
                heroTracker.weaponSlot.instance = 0;
            }
        }
    }

    state.overlayMeta = ranged_anim::PackOverlayMeta(meta);
    ranged_anim::PackOverlayHot(hot, state.flags, state.overlayMeta, state.gold, state.actionMs);
}

void PresentPuppetOverlay(eCEntity* puppetEntity, char const* anim0, char const* anim1, char const* weaponAnim,
                          uint32_t overlayMeta, ranged_anim::OverlayHot const& hot)
{
    if (!puppetEntity) return;
    if (overlayMeta == 0) {
        ClearPuppetOverlay(puppetEntity);
        return;
    }
    auto* animation = static_cast<eCVisualAnimation_PS*>(puppetEntity->GetPropertySet(eEPropertySetType_Animation));
    auto* actor = animation ? animation->GetActor() : nullptr;
    if (!actor) return;

    PuppetOverlayState& state = puppetOverlays[puppetEntity->GetID()];
    state.entityProxy = eCEntityProxy(puppetEntity);
    const ranged_anim::OverlayMeta meta = ranged_anim::UnpackOverlayMeta(overlayMeta);
    PresentOverlayChannels(animation, actor, state, meta, overlayMeta, anim0, anim1, hot);
    PresentWeaponMotion(puppetEntity, state, meta, weaponAnim, hot);
}

void ClearPuppetOverlay(eCEntity* puppetEntity)
{
    if (!puppetEntity) return;
    const auto it = puppetOverlays.find(puppetEntity->GetID());
    if (it == puppetOverlays.end()) return;
    PuppetOverlayState& state = it->second;
    if (state.entityProxy.GetEntity() == puppetEntity && state.drivenMask != 0) {
        auto* animation = static_cast<eCVisualAnimation_PS*>(puppetEntity->GetPropertySet(eEPropertySetType_Animation));
        auto* actor = animation ? animation->GetActor() : nullptr;
        if (animation && actor) {
            for (int s = 1; s <= 7; ++s) {
                if ((state.drivenMask & (1u << s)) == 0) continue;
                StopAnimationSlot(animation, actor, static_cast<eCWrapper_emfx2Actor::eEMotionType>(s), 0.08f);
            }
        }
    }
    StopDrivenWeapon(state);
    puppetOverlays.erase(it);
}

void ResetPuppetOverlays()
{
    puppetOverlays.clear();
    heroTracker = HeroOverlayTracker{};
}
} // namespace g3t::ranged
