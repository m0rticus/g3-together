// SPDX-License-Identifier: GPL-3.0-or-later
// Engine-independent wire codec, timeline history, and slot-sync policy for all player weapon and worn slots.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace g3t::player_equip {

inline constexpr uint8_t Version = 1;
inline constexpr size_t MaxItemName = 64;
inline constexpr uint16_t MaxQuality = 1023;

enum SlotIndex : uint8_t {
    SlotRightHand = 0,
    SlotLeftHand = 1,
    SlotEquipLeftHand = 2,
    SlotBackLeft = 3,
    SlotBackRight = 4,
    SlotBow = 5,
    SlotCrossbow = 6,
    SlotArmor = 7,
    SlotBelt = 8,
    SlotAmmo = 9,
    SlotBody = 10,
    SlotHelmet = 11,
    SlotCount = 12
};

struct SlotSpec {
    uint8_t index;
    uint8_t nativeSlot;
    bool weaponSlot;
    char const* label;
};

inline constexpr SlotSpec Slots[SlotCount] = {
    {SlotRightHand, 1, true, "RightHand"},
    {SlotLeftHand, 2, true, "LeftHand"},
    {SlotEquipLeftHand, 3, true, "EquipLeftHand"},
    {SlotBackLeft, 5, true, "BackLeft"},
    {SlotBackRight, 6, true, "BackRight"},
    {SlotBow, 7, true, "Bow"},
    {SlotCrossbow, 8, true, "Crossbow"},
    {SlotArmor, 9, false, "Armor"},
    {SlotBelt, 10, true, "Belt"},
    {SlotAmmo, 14, true, "Ammo"},
    {SlotBody, 17, false, "Body"},
    {SlotHelmet, 18, false, "Helmet"},
};

struct SlotItem {
    char name[MaxItemName] = {};
    uint16_t quality = 0;

    bool Empty() const noexcept { return name[0] == '\0'; }

    bool operator==(SlotItem const& other) const noexcept
    {
        if (Empty() && other.Empty()) return true;
        return quality == other.quality && std::strncmp(name, other.name, MaxItemName) == 0;
    }

    bool operator!=(SlotItem const& other) const noexcept { return !(*this == other); }
};

inline void CopyItemName(char (&dst)[MaxItemName], char const* src) noexcept
{
    std::memset(dst, 0, sizeof(dst));
    if (!src || !*src) return;
    size_t i = 0;
    for (; i + 1 < MaxItemName && src[i] != '\0'; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

struct PlayerEquipment {
    uint8_t id = 0;
    uint32_t sequence = 0;
    uint32_t sent = 0;
    uint32_t world = 0;
    uint32_t epoch = 0;
    bool transformed = false;
    SlotItem slots[SlotCount]{};

    void SetSlot(size_t index, char const* name, uint16_t quality = 0) noexcept
    {
        if (index >= SlotCount) return;
        CopyItemName(slots[index].name, name);
        slots[index].quality = slots[index].Empty() ? 0 : static_cast<uint16_t>(quality & MaxQuality);
    }

    bool SameSlots(PlayerEquipment const& other) const noexcept
    {
        if (transformed != other.transformed) return false;
        for (size_t i = 0; i < SlotCount; ++i)
            if (slots[i] != other.slots[i]) return false;
        return true;
    }
};

namespace detail {

inline bool BoundedString(char const* data, size_t capacity, size_t& lenOut) noexcept
{
    for (size_t i = 0; i < capacity; ++i) {
        if (data[i] == '\0') {
            lenOut = i;
            return true;
        }
    }
    return false;
}

inline void PushU8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }
inline void PushU16(std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
}
inline void PushU32(std::vector<uint8_t>& out, uint32_t v)
{
    PushU16(out, static_cast<uint16_t>(v));
    PushU16(out, static_cast<uint16_t>(v >> 16));
}

struct Reader {
    uint8_t const* data;
    size_t size;
    size_t offset = 0;
    bool ok = true;

    uint8_t U8() noexcept
    {
        if (offset >= size) { ok = false; return 0; }
        return data[offset++];
    }
    uint16_t U16() noexcept
    {
        const uint8_t lo = U8();
        const uint8_t hi = U8();
        return static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8));
    }
    uint32_t U32() noexcept
    {
        const uint16_t lo = U16();
        const uint16_t hi = U16();
        return static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 16);
    }
};

} // namespace detail

inline constexpr size_t FixedWireBytes = 21;

inline bool Encode(PlayerEquipment const& equip, std::vector<uint8_t>& out)
{
    out.clear();
    if (equip.id < 1 || equip.id > 7) return false;
    uint16_t mask = 0;
    size_t lengths[SlotCount]{};
    size_t totalBytes = FixedWireBytes;
    for (size_t i = 0; i < SlotCount; ++i) {
        if (equip.slots[i].quality > MaxQuality) return false;
        if (!detail::BoundedString(equip.slots[i].name, MaxItemName, lengths[i])) return false;
        if (lengths[i] == 0) {
            if (equip.slots[i].quality != 0) return false;
            continue;
        }
        mask |= static_cast<uint16_t>(1u << i);
        totalBytes += 3u + lengths[i];
    }
    out.reserve(totalBytes);
    detail::PushU8(out, Version);
    detail::PushU8(out, equip.id);
    detail::PushU8(out, equip.transformed ? 1u : 0u);
    detail::PushU32(out, equip.sequence);
    detail::PushU32(out, equip.sent);
    detail::PushU32(out, equip.world);
    detail::PushU32(out, equip.epoch);
    detail::PushU16(out, mask);
    for (size_t i = 0; i < SlotCount; ++i) {
        if ((mask & static_cast<uint16_t>(1u << i)) == 0) continue;
        detail::PushU16(out, equip.slots[i].quality);
        detail::PushU8(out, static_cast<uint8_t>(lengths[i]));
        out.insert(out.end(), equip.slots[i].name, equip.slots[i].name + lengths[i]);
    }
    return true;
}

inline bool Decode(uint8_t const* data, size_t size, PlayerEquipment& out, uint8_t authoritativeId = 0)
{
    out = PlayerEquipment{};
    if (!data || size < FixedWireBytes) return false;
    detail::Reader r{data, size};
    const uint8_t version = r.U8();
    const uint8_t id = r.U8();
    const uint8_t flags = r.U8();
    const uint32_t sequence = r.U32();
    const uint32_t sent = r.U32();
    const uint32_t worldHash = r.U32();
    const uint32_t epoch = r.U32();
    const uint16_t mask = r.U16();
    if (!r.ok || version != Version || id < 1 || id > 7 || (flags & ~1u) != 0 || (mask >> SlotCount) != 0)
        return false;
    if (authoritativeId != 0 && (authoritativeId < 1 || authoritativeId > 7 || authoritativeId != id))
        return false;

    out.id = id;
    out.transformed = (flags & 1u) != 0;
    out.sequence = sequence;
    out.sent = sent;
    out.world = worldHash;
    out.epoch = epoch;

    for (size_t i = 0; i < SlotCount; ++i) {
        if ((mask & static_cast<uint16_t>(1u << i)) == 0) continue;
        const uint16_t quality = r.U16();
        const uint8_t len = r.U8();
        if (!r.ok || quality > MaxQuality || len == 0 || len >= MaxItemName) return false;
        for (uint8_t j = 0; j < len; ++j) {
            const uint8_t ch = r.U8();
            if (!r.ok || ch == 0) return false;
            out.slots[i].name[j] = static_cast<char>(ch);
        }
        out.slots[i].name[len] = '\0';
        out.slots[i].quality = quality;
    }
    return r.ok && r.offset == size;
}

inline bool PeekPlayerId(uint8_t const* data, size_t size, uint8_t& idOut) noexcept
{
    if (!data || size < 2) return false;
    idOut = data[1];
    return idOut >= 1 && idOut <= 7;
}

struct EquipHistory {
    static constexpr size_t Capacity = 64;
    std::array<PlayerEquipment, Capacity> entries{};
    size_t count = 0;
    uint32_t serial = 0;

    void Reset(uint32_t newSerial = 0) noexcept
    {
        count = 0;
        serial = newSerial;
    }

    void Push(uint32_t trackSerial, PlayerEquipment const& sample) noexcept
    {
        if (serial != trackSerial) Reset(trackSerial);
        PlayerEquipment entry = sample;
        if (count > 0) {
            const int32_t dt = static_cast<int32_t>(entry.sent - entries[count - 1].sent);
            if (dt < 0) entry.sent = entries[count - 1].sent;
            if (entry.sent == entries[count - 1].sent) {
                entries[count - 1] = entry;
                return;
            }
        }
        if (count == Capacity) {
            for (size_t i = 1; i < Capacity; ++i) entries[i - 1] = entries[i];
            --count;
        }
        entries[count++] = entry;
    }

    bool Sample(uint32_t renderSent, bool hasAppliedFirst, PlayerEquipment& out) const noexcept
    {
        if (count == 0) return false;
        if (!hasAppliedFirst && count == 1) {
            out = entries[0];
            return true;
        }
        for (size_t i = count; i > 0; --i) {
            if (static_cast<int32_t>(renderSent - entries[i - 1].sent) >= 0) {
                out = entries[i - 1];
                return true;
            }
        }
        out = entries[0];
        return true;
    }
};

inline SlotItem DesiredSlotItem(PlayerEquipment const& wanted, size_t index, bool applyWeapons, bool applyWear) noexcept
{
    if (index >= SlotCount || wanted.transformed) return SlotItem{};
    if (Slots[index].weaponSlot ? !applyWeapons : !applyWear) return SlotItem{};
    return wanted.slots[index];
}

template <typename Ops>
void UnlinkMismatchedSlots(Ops& ops, PlayerEquipment const& wanted, bool applyWeapons, bool applyWear)
{
    for (size_t i = 0; i < SlotCount; ++i) {
        const uint8_t nativeSlot = Slots[i].nativeSlot;
        const SlotItem desired = DesiredSlotItem(wanted, i, applyWeapons, applyWear);
        const SlotItem current = ops.ReadSlot(nativeSlot);
        if (!current.Empty() && current != desired)
            ops.UnlinkSlot(nativeSlot);
    }
}

template <typename Ops>
void ApplyMissingSlots(Ops& ops, PlayerEquipment const& wanted, bool applyWeapons, bool applyWear,
                       SlotItem (&failed)[SlotCount])
{
    for (size_t i = 0; i < SlotCount; ++i) {
        const uint8_t nativeSlot = Slots[i].nativeSlot;
        const SlotItem desired = DesiredSlotItem(wanted, i, applyWeapons, applyWear);
        if (desired.Empty()) {
            failed[i] = SlotItem{};
            continue;
        }
        const SlotItem current = ops.ReadSlot(nativeSlot);
        if (current == desired) {
            failed[i] = SlotItem{};
            continue;
        }
        if (!failed[i].Empty() && failed[i] == desired) continue;
        if (!current.Empty()) ops.UnlinkSlot(nativeSlot);
        const int stack = ops.AcquireUnlinkedStack(desired.name, desired.quality, nativeSlot);
        if (stack >= 0 && ops.LinkToSlot(stack, nativeSlot))
            failed[i] = SlotItem{};
        else
            failed[i] = desired;
    }
    ops.DisableHandCollision(Slots[SlotRightHand].nativeSlot);
    ops.DisableHandCollision(Slots[SlotLeftHand].nativeSlot);
}

template <typename Ops>
void SyncSlots(Ops& ops, PlayerEquipment const& wanted, bool applyWeapons, bool applyWear,
               SlotItem (&failed)[SlotCount])
{
    UnlinkMismatchedSlots(ops, wanted, applyWeapons, applyWear);
    ApplyMissingSlots(ops, wanted, applyWeapons, applyWear, failed);
}

} // namespace g3t::player_equip
