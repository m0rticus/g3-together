// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace g3t::npcwire::control_policy {

struct Reference {
    std::uint16_t id = 0, baseline = 0;
    std::uint32_t incarnation = 0, anchor = 0;
};

inline bool SameReference(Reference const& a, Reference const& b)
{
    return a.id == b.id && a.baseline == b.baseline &&
           a.incarnation == b.incarnation && a.anchor == b.anchor;
}

struct BaselinePin {
    Reference reference{};
    std::uint8_t lifeFlags = 0;
    bool valid = false;
};

inline bool SameLifeDefinition(BaselinePin const& pin, Reference const& current,
                              std::uint32_t incarnation, std::uint8_t lifeFlags)
{
    return pin.valid && SameReference(pin.reference, current) &&
           pin.reference.incarnation == incarnation && pin.lifeFlags == lifeFlags;
}

inline bool ShouldAllocateBaseline(bool hasCurrent, bool lifeChanged,
                                   bool baselineRetained, bool pendingSameLife,
                                   bool encodeFailed)
{
    if (!hasCurrent || lifeChanged) return true;
    if (pendingSameLife) return false;
    return !baselineRetained || encodeFailed;
}

struct OfferTransaction {
    Reference reference{};
    std::uint32_t serial = 0;
    bool idQueued = false;
    bool baselineQueued = false;
    bool acknowledged = false;
};

inline bool AcceptAck(OfferTransaction const& offer, Reference const& reference,
                     std::uint32_t serial)
{
    return offer.idQueued && offer.baselineQueued &&
           offer.serial == serial && SameReference(offer.reference, reference);
}

inline bool RetryOffer(OfferTransaction const& offer, Reference const& reference)
{
    return offer.serial != 0 && SameReference(offer.reference, reference) &&
           !offer.acknowledged;
}

inline bool ShouldSendId(OfferTransaction const& offer)
{
    return !offer.idQueued;
}

inline bool ShouldSendBaseline(OfferTransaction const& offer)
{
    return !offer.baselineQueued;
}

enum class ResyncReason : std::uint8_t {
    None,
    UnknownId,
    UnknownBaseline,
    UnknownAnchor,
    Invalid
};

struct ResyncCursor {
    Reference reference{};
    ResyncReason reason = ResyncReason::None;
    bool handled = false;
};

inline bool DuplicateResync(ResyncCursor const& cursor, Reference const& reference,
                            ResyncReason reason)
{
    return cursor.handled && SameReference(cursor.reference, reference) &&
           cursor.reason == reason;
}

inline bool RetryAt(std::uint32_t now, std::uint32_t attemptedAt,
                    std::uint32_t interval)
{
    return static_cast<std::uint32_t>(now - attemptedAt) >= interval;
}

} // namespace g3t::npcwire::control_policy
