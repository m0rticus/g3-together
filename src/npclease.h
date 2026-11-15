// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "npclease_wire.h"

namespace g3t::npclease {

struct Interest {
    bool sameWorld = true, ready = true, eligible = true, conflict = false, hostPresent = true;
    float hostDistance = 0, clientDistance = 0, hostRoi = 8000;

    bool ValidMetrics() const
    {
        return std::isfinite(hostDistance) && std::isfinite(clientDistance) &&
               std::isfinite(hostRoi) && hostRoi >= 0 &&
               hostDistance >= 0 && clientDistance >= 0;
    }

    bool EligibleState() const
    {
        return sameWorld && ready && eligible && !conflict;
    }

    float FarRadius() const
    {
        return hostRoi + 2000.0f > 12000.0f ? hostRoi + 2000.0f : 12000.0f;
    }
};

struct Eligibility {
    uint32_t farSince = 0;
    bool farHeld = false;
    bool dwellActive = false;

    bool Delegate(Interest const& interest, uint32_t now)
    {
        if (!interest.EligibleState() || !interest.ValidMetrics()) {
            farSince = 0;
            farHeld = false;
            dwellActive = false;
            return false;
        }

        const float farRadius = interest.FarRadius();
        const float recallRadius = farRadius - 2000.0f;

        if (farHeld && interest.hostPresent && interest.hostDistance <= recallRadius) {
            farSince = 0;
            farHeld = false;
            dwellActive = false;
            return false;
        }
        if (interest.clientDistance > ClientRadiusCm) {
            farSince = 0;
            dwellActive = false;
            return false;
        }
        if (!interest.hostPresent) {
            farSince = 0;
            farHeld = true;
            dwellActive = false;
            return true;
        }
        if (farHeld) {
            return true;
        }

        if (interest.hostDistance >= farRadius) {
            if (!dwellActive) {
                farSince = now;
                dwellActive = true;
            }
            if (Reached(now, farSince + FarDwellMs)) {
                farHeld = true;
                dwellActive = false;
                return true;
            }
        } else {
            farSince = 0;
            dwellActive = false;
        }
        return false;
    }

    bool Recall(Interest const& interest) const
    {
        if (!interest.ValidMetrics() || !interest.EligibleState()) return true;
        if (!interest.hostPresent) return false;
        return interest.hostDistance <= interest.FarRadius() - 2000.0f;
    }
};

// Why Eligibility::Delegate refused (diagnostics only; the order mirrors Delegate).
inline char const* DelegateRefusal(Interest const& interest)
{
    if (!interest.sameWorld) return "client not in the host's world";
    if (!interest.ready) return "client state stale or checkpoint busy";
    if (!interest.eligible) return "host authority guards unavailable";
    if (interest.conflict) return "another client shares this NPC";
    if (interest.clientDistance > ClientRadiusCm) return "NPC outside the client ring";
    return "host presents it within its own reach";
}

struct Lease {
    Phase phase = Phase::Host;
    uint32_t epoch = 0, incarnation = 1, generation = 0, revision = 0, deadline = 0, started = 0;
    uint8_t owner = 1;
    Checkpoint state{};

    bool Offer(uint8_t client, uint32_t peerGeneration, uint32_t nextEpoch, uint32_t now, Checkpoint const& baseline)
    {
        if ((phase != Phase::Host && phase != Phase::Dormant) ||
            client >= 8 ||
            !Newer(nextEpoch, epoch) || !Valid(baseline)) return false;
        if (state.dead && !baseline.dead) return false;
        if ((state.effects & static_cast<uint8_t>(~baseline.effects)) != 0) return false;
        phase = Phase::Offering;
        epoch = nextEpoch;
        generation = peerGeneration;
        revision = 0;
        deadline = now + PrepareMs;
        started = now;
        owner = client;
        state = baseline;
        return true;
    }

    bool Prepared(uint32_t authorityEpoch, uint32_t now)
    {
        if (phase != Phase::Offering || authorityEpoch != epoch ||
            Reached(now, deadline)) return false;
        phase = Phase::Client;
        deadline = now + LeaseMs + LeaseSafetyMs;
        started = now;
        return true;
    }

    bool CanAcceptClientState(uint8_t client, uint32_t peerGeneration, uint32_t authorityEpoch,
                              uint32_t life, Checkpoint const& value) const
    {
        if ((phase != Phase::Client && phase != Phase::Recalling) ||
            client != owner || peerGeneration != generation ||
            authorityEpoch != epoch || life != incarnation || !Valid(value)) return false;
        if (state.dead && !value.dead) return false;
        return (state.effects & static_cast<uint8_t>(~value.effects)) == 0;
    }

    bool AcceptState(uint8_t client, uint32_t peerGeneration, uint32_t authorityEpoch,
                     uint32_t life, uint32_t nextRevision, Checkpoint const& value)
    {
        if (!CanAcceptClientState(client, peerGeneration, authorityEpoch, life, value)) return false;
        if (Newer(nextRevision, revision)) {
            revision = nextRevision;
            state = value;
            return true;
        }
        return false;
    }

    bool Recall(uint32_t now)
    {
        if (phase != Phase::Client) return false;
        phase = Phase::Recalling;
        started = now;
        return true;
    }

    bool Cut(uint8_t client, uint32_t peerGeneration, uint32_t authorityEpoch,
             uint32_t life, uint32_t nextRevision, Checkpoint const& value)
    {
        if (!CanAcceptClientState(client, peerGeneration, authorityEpoch, life, value)) return false;

        if (Newer(nextRevision, revision)) {
            revision = nextRevision;
            state = value;
            phase = Phase::Frozen;
            return true;
        }
        if (nextRevision == revision && detail::ExactState(state, value)) {
            phase = Phase::Frozen;
            return true;
        }
        return false;
    }

    bool HandBack(uint32_t nextEpoch, uint32_t now)
    {
        if (phase != Phase::Frozen || !Newer(nextEpoch, epoch)) return false;
        epoch = nextEpoch;
        generation = 0;
        deadline = 0;
        started = now;
        owner = 1;
        phase = Phase::Host;
        return true;
    }

    bool Recover(uint32_t now)
    {
        if (phase == Phase::Offering) {
            if (!Reached(now, deadline)) return false;
            phase = Phase::Host;
            generation = 0;
            deadline = 0;
            started = 0;
            owner = 1;
            return true;
        }
        if (phase == Phase::Client || phase == Phase::Recalling) {
            if (!Reached(now, deadline)) return false;
            phase = Phase::Dormant;
            generation = 0;
            deadline = 0;
            started = 0;
            owner = 1;
            return true;
        }
        return false;
    }

    bool ActiveLeases() const
    {
        return phase == Phase::Offering || phase == Phase::Client ||
               phase == Phase::Recalling || phase == Phase::Frozen;
    }
};

inline bool CommitEffect(Checkpoint& state, Effect effect)
{
    if (effect != NativeDeath && effect != QuestDeath) return false;
    const uint8_t bit = static_cast<uint8_t>(effect);
    if ((state.effects & bit) != 0) return false;
    state.effects = static_cast<uint8_t>(state.effects | bit);
    if (effect == NativeDeath) state.dead = 1;
    return true;
}

struct Saved {
    uint8_t guid[16]{};
    uint32_t incarnation = 1, epoch = 0, revision = 0;
    Checkpoint state{};
    uint8_t adopted = 0;
};

using SaveOverlay = std::vector<Saved>;

namespace detail {

constexpr uint32_t OverlayMagic = 0x314c504eu;
constexpr uint32_t OverlayVersion = 2, LegacyOverlayVersion = 1;
constexpr uint32_t LegacySavedWireSize = 16u + 4u + 4u + 4u + sizeof(Checkpoint);
constexpr uint32_t SavedWireSize = LegacySavedWireSize + 1u;

inline uint32_t Crc32Update(uint32_t crc, uint8_t const* bytes, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
}

inline uint32_t OverlayCrc32(uint8_t const* bytes, size_t size)
{
    uint32_t crc = 0xffffffffu;
    crc = Crc32Update(crc, bytes, 16);
    crc = Crc32Update(crc, bytes + 20, size - 20);
    return crc ^ 0xffffffffu;
}

inline bool ValidSavedEntry(Saved const& saved)
{
    return GuidNonzero(saved.guid) && saved.incarnation && saved.epoch &&
           saved.adopted <= 1 && Valid(saved.state);
}

} // namespace detail

inline bool EncodeOverlay(SaveOverlay const& overlay, std::vector<uint8_t>& output)
{
    if (overlay.size() > MaxLeases) return false;
    if (overlay.size() > (SIZE_MAX - 20) / detail::SavedWireSize) return false;

    for (size_t i = 0; i < overlay.size(); ++i) {
        const Saved& saved = overlay[i];
        if (!detail::ValidSavedEntry(saved)) return false;
        for (size_t j = 0; j < i; ++j)
            if (std::memcmp(saved.guid, overlay[j].guid, 16) == 0) return false;
    }

    const size_t payloadSize = overlay.size() * detail::SavedWireSize;
    std::vector<uint8_t> encoded(20 + payloadSize);
    detail::Put32(encoded, 0, detail::OverlayMagic);
    detail::Put32(encoded, 4, detail::OverlayVersion);
    detail::Put32(encoded, 8, static_cast<uint32_t>(overlay.size()));
    detail::Put32(encoded, 12, static_cast<uint32_t>(payloadSize));
    detail::Put32(encoded, 16, 0);

    size_t offset = 20;
    for (Saved const& saved : overlay) {
        std::memcpy(encoded.data() + offset, saved.guid, 16); offset += 16;
        detail::Put32(encoded, offset, saved.incarnation); offset += 4;
        detail::Put32(encoded, offset, saved.epoch); offset += 4;
        detail::Put32(encoded, offset, saved.revision); offset += 4;
        offset = detail::WriteCheckpoint(encoded, offset, saved.state);
        encoded[offset++] = saved.adopted;
    }

    detail::Put32(encoded, 16, detail::OverlayCrc32(encoded.data(), encoded.size()));
    output.swap(encoded);
    return true;
}

inline bool DecodeOverlay(uint8_t const* data, size_t size, SaveOverlay& output)
{
    if (!data || size < 20) return false;
    if (detail::Get32(data) != detail::OverlayMagic) return false;
    const uint32_t version = detail::Get32(data + 4);
    if (version != detail::OverlayVersion && version != detail::LegacyOverlayVersion) return false;
    const uint32_t rowSize = version == detail::OverlayVersion ?
                             detail::SavedWireSize : detail::LegacySavedWireSize;

    const uint32_t count = detail::Get32(data + 8);
    const uint32_t payloadSize = detail::Get32(data + 12);
    if (count > MaxLeases) return false;
    if (payloadSize != uint64_t(count) * rowSize) return false;
    if (size != size_t(20) + size_t(payloadSize)) return false;

    if (detail::OverlayCrc32(data, size) != detail::Get32(data + 16)) return false;

    SaveOverlay decoded;
    decoded.reserve(count);
    size_t offset = 20;
    for (uint32_t i = 0; i < count; ++i) {
        Saved saved;
        std::memcpy(saved.guid, data + offset, 16); offset += 16;
        saved.incarnation = detail::Get32(data + offset); offset += 4;
        saved.epoch = detail::Get32(data + offset); offset += 4;
        saved.revision = detail::Get32(data + offset); offset += 4;
        offset = detail::ReadCheckpoint(data, offset, saved.state);
        if (version == detail::OverlayVersion) saved.adopted = data[offset++];
        if (!detail::ValidSavedEntry(saved)) return false;
        for (Saved const& prior : decoded)
            if (std::memcmp(prior.guid, saved.guid, 16) == 0) return false;
        decoded.push_back(saved);
    }

    output.swap(decoded);
    return true;
}

} // namespace g3t::npclease
