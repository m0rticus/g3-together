// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npc_state.h"
#include "npcsend_candidates.h"

#include <unordered_set>

namespace g3t::npcs::detail {

struct HostSendCandidateEntity {
    eCEntity* instance = nullptr;
    bool ambiguous = false;
    bool fromProcessed = false;
};

struct HostSendFrame {
    struct Entry {
        GuidKey key{};
        HostSendCandidateEntity value{};
    };
    struct KeySlot {
        uint32_t epoch = 0;
        uint32_t entryIndex = 0;
    };
    struct PtrSlot {
        eCEntity* ptr = nullptr;
        uint32_t epoch = 0;
        uint32_t entryIndex = 0;
    };

    std::vector<Entry> entries;
    std::vector<KeySlot> keySlots;
    std::vector<PtrSlot> ptrSlots;
    uint32_t epoch = 1;
    eCEntity* partyLeaderOf[net::MaxPlayers] = {};
    bool recipientHere[net::MaxPlayers] = {};
    bool anyRecipientHere = false;
    uint32_t stretchOf[net::MaxPlayers] = {};

    static size_t PtrHash(eCEntity* ptr) noexcept
    {
        return (reinterpret_cast<uintptr_t>(ptr) >> 4) * 2654435761u;
    }

    void Clear()
    {
        entries.clear();
        if (++epoch == 0) {
            for (auto& s : keySlots) s.epoch = 0;
            for (auto& s : ptrSlots) s.epoch = 0;
            epoch = 1;
        }
        std::fill(std::begin(partyLeaderOf), std::end(partyLeaderOf), nullptr);
        std::fill(std::begin(recipientHere), std::end(recipientHere), false);
        anyRecipientHere = false;
        std::fill(std::begin(stretchOf), std::end(stretchOf), 0u);
    }

    void Rehash(size_t capacity)
    {
        keySlots.assign(capacity, KeySlot{});
        ptrSlots.assign(capacity, PtrSlot{});
        const size_t mask = capacity - 1;
        const GuidHash hasher{};
        for (uint32_t i = 0; i < static_cast<uint32_t>(entries.size()); ++i) {
            size_t kSlot = hasher(entries[i].key) & mask;
            while (keySlots[kSlot].epoch == epoch) kSlot = (kSlot + 1) & mask;
            keySlots[kSlot] = {epoch, i};

            eCEntity* ptr = entries[i].value.instance;
            size_t pSlot = PtrHash(ptr) & mask;
            while (ptrSlots[pSlot].epoch == epoch) pSlot = (pSlot + 1) & mask;
            ptrSlots[pSlot] = {ptr, epoch, i};
        }
    }

    void EnsureCapacity()
    {
        if (keySlots.empty()) {
            Rehash(512);
        } else if ((entries.size() + 1) * 2 >= keySlots.size()) {
            Rehash(keySlots.size() * 2);
        }
    }

    bool FindByPtr(eCEntity* ptr, bool markFromProcessed, GuidKey& outKey)
    {
        if (ptrSlots.empty() || entries.empty() || !ptr) return false;
        const size_t mask = ptrSlots.size() - 1;
        size_t slot = PtrHash(ptr) & mask;
        while (ptrSlots[slot].epoch == epoch) {
            if (ptrSlots[slot].ptr == ptr) {
                Entry& e = entries[ptrSlots[slot].entryIndex];
                if (markFromProcessed) e.value.fromProcessed = true;
                outKey = e.key;
                return true;
            }
            slot = (slot + 1) & mask;
        }
        return false;
    }

    void NoteCandidate(GuidKey const& key, eCEntity* instance, bool fromProcessed)
    {
        if (!instance || IsZeroGuid(key)) return;
        EnsureCapacity();
        const size_t mask = keySlots.size() - 1;
        size_t slot = GuidHash{}(key) & mask;
        while (keySlots[slot].epoch == epoch) {
            Entry& existing = entries[keySlots[slot].entryIndex];
            if (existing.key == key) {
                if (existing.value.instance != instance) existing.value.ambiguous = true;
                if (fromProcessed) existing.value.fromProcessed = true;
                return;
            }
            slot = (slot + 1) & mask;
        }
        const uint32_t idx = static_cast<uint32_t>(entries.size());
        entries.push_back({key, {instance, false, fromProcessed}});
        keySlots[slot] = {epoch, idx};
        size_t pSlot = PtrHash(instance) & mask;
        while (ptrSlots[pSlot].epoch == epoch && ptrSlots[pSlot].ptr != instance) pSlot = (pSlot + 1) & mask;
        if (ptrSlots[pSlot].epoch != epoch) ptrSlots[pSlot] = {instance, epoch, idx};
    }

    HostSendCandidateEntity const* FindEntity(GuidKey const& key) const noexcept
    {
        if (keySlots.empty() || entries.empty()) return nullptr;
        const size_t mask = keySlots.size() - 1;
        size_t slot = GuidHash{}(key) & mask;
        while (keySlots[slot].epoch == epoch) {
            Entry const& existing = entries[keySlots[slot].entryIndex];
            if (existing.key == key) return &existing.value;
            slot = (slot + 1) & mask;
        }
        return nullptr;
    }
};

int OnRelease(uint8_t owner, uint32_t generation, uint8_t* guids, int count);
void OnMotionName(uint32_t hash, char const* name);
uint8_t FightFlags(Entity& npc);
void PrepareHostSendFrame(std::vector<npcsend_candidates::GuidCandidate> const& candidates,
                          size_t firstProcessed, unsigned candidateKeyCount, unsigned processedCount,
                          HostSendFrame& out);
eCEntity* AdmitHostSendCandidate(GuidKey const& candidateKey, eCEntity* hero, eCEntity* controlled,
                                 HostSendFrame const& frame, float const players[][3], int playerCount,
                                 uint8_t outGuid[16], GuidKey& outKey, float outAt[3],
                                 LastSent*& outExistingLast);
bool CheckHostSendRange(eCEntity* instance, GuidKey const& key, float const at[3],
                        float const players[][3], int playerCount, LastSent const* existingLast,
                        SendNote& note);
LastSent& CaptureAndRecordOwnedNpc(Entity& self, eCEntity* instance, GuidKey const& key,
                                   float const at[3], HostSendFrame const& frame,
                                   LastSent* existingLast, net::NpcRecord& r,
                                   std::vector<pose::Bone>& bones, npcsend::PoseMeta& poseMeta,
                                   bool& poseCaptured, uint8_t& poseFlags, bool& posed,
                                   bool& moving, bool& outsideAll);
void SendNpcToRecipients(Entity& self, eCEntity* instance, eCEntity* hostFocus, eCEntity* hostTarget,
                         GuidKey const& key, uint8_t const guid[16], float const at[3], net::NpcRecord const& r,
                         LastSent& last, HostSendFrame const& frame, std::vector<pose::Bone>& bones,
                         npcsend::PoseMeta const& poseMeta, bool poseCaptured, uint8_t poseFlags, bool posed,
                         bool moving, bool outsideAll);
void FinishOwnedTick(eCEntity* hero, float const players[][3], int playerCount);
void HostTick(void (*sendOwned)(eCEntity*, float const[][3], int));

} // namespace g3t::npcs::detail
