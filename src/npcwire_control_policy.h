// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

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

inline bool AlreadySatisfied(OfferTransaction const& offer, Reference const& reference,
                             bool force)
{
    return !force && offer.serial != 0 && offer.acknowledged &&
           offer.idQueued && offer.baselineQueued &&
           SameReference(offer.reference, reference);
}

inline bool BeginOffer(OfferTransaction& offer, Reference const& reference,
                       bool force, std::uint32_t& nextSerial)
{
    if (AlreadySatisfied(offer, reference, force)) return true;
    const bool retry = !force && RetryOffer(offer, reference);
    if (!retry) {
        if (nextSerial == (std::numeric_limits<std::uint32_t>::max)()) return false;
        offer.reference = reference;
        offer.serial = ++nextSerial;
        offer.idQueued = false;
        offer.baselineQueued = false;
        offer.acknowledged = false;
    }
    return true;
}

inline bool CompleteAck(OfferTransaction& offer, Reference const& reference,
                        std::uint32_t serial)
{
    if (!AcceptAck(offer, reference, serial)) return false;
    offer.acknowledged = true;
    return true;
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

inline bool CommitResync(ResyncCursor& cursor, Reference const& reference,
                         ResyncReason reason, bool offerQueued)
{
    if (!offerQueued) return false;
    cursor.reference = reference;
    cursor.reason = reason;
    cursor.handled = true;
    return true;
}

inline bool RetryAt(std::uint32_t now, std::uint32_t attemptedAt,
                    std::uint32_t interval)
{
    return static_cast<std::uint32_t>(now - attemptedAt) >= interval;
}

} // namespace g3t::npcwire::control_policy
