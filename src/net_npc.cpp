// SPDX-License-Identifier: GPL-3.0-or-later
#include "net_internal.h"

#include "log.h"
#include "npclease_presentation.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace g3t::net::internal {

uint32_t npcSequence = 0;
uint32_t npcPublication = 0;
std::unordered_map<std::string, replication::Sequence> npcOrigins[MaxPlayers];
std::unordered_map<NpcKeyValue, replication::ReplicaOrder, NpcKeyHash> npcPublications;
unsigned npcAccepted = 0, npcRejected = 0, npcStale = 0;
unsigned controlErrors = 0, npcControlFallback = 0;
npcwire::Context controlSender, controlReceiver;
uint32_t receivedControlGeneration = 0;
std::unordered_map<NpcKeyValue, ControlState, NpcKeyHash> controlStates;
unsigned npcControlCreated = 0, npcControlAckAccepted = 0;
uint64_t npcOrderedSetupPoses = 0, npcOrderedSetupControls = 0, npcAnchorsSent = 0, npcAnchorsReceived = 0;

namespace {
uint32_t controlGeneration = 0, nextControlId = 0;
uint32_t nextControlOffer = 0;
bool controlGenerationExhausted = false, controlIdExhaustedLogged = false;
struct RequestedControlEntry {
    uint32_t generation = 0;
    uint32_t anchor = 0;
    uint16_t baseline = 0;
    npcwire::ResyncReason reason = npcwire::ResyncReason::None;
    bool operator==(RequestedControlEntry const& other) const noexcept
    {
        return generation == other.generation && anchor == other.anchor &&
               baseline == other.baseline && reason == other.reason;
    }
};
std::unordered_map<uint16_t, NpcKeyValue> controlKeys;
std::unordered_map<uint16_t, ControlOffer> controlOffered[MaxPlayers];
std::unordered_map<uint16_t, npcwire::BaselineRef> controlAcknowledged[MaxPlayers];
std::unordered_map<uint16_t, RequestedControlEntry> requestedControls;
unsigned controlBaselineLogs = 0, controlRequestLogs = 0, controlBatchLogs = 0;
unsigned npcControlPending = 0, npcControlStale = 0, npcControlResync = 0, npcControlPartial = 0;

bool SameControlRef(npcwire::BaselineRef const& a, npcwire::BaselineRef const& b)
{
    return a.id == b.id && a.baseline == b.baseline && a.anchor == b.anchor && a.incarnation == b.incarnation;
}

npcwire::control_policy::Reference ToPolicyRef(npcwire::BaselineRef const& ref)
{
    return {ref.id, ref.baseline, ref.incarnation, ref.anchor};
}

npcwire::control_policy::OfferTransaction ToPolicyOffer(ControlOffer const& offer)
{
    return {ToPolicyRef(offer.ref), offer.serial, offer.idQueued, offer.baselineQueued, offer.acknowledged};
}

bool ControlStream(uint8_t const* data, size_t size, uint8_t kind, npcwire::Stream& stream)
{
    if (!data || size < sizeof(npcwire::WireBatchHeader) || data[0] != 'N' || data[1] != kind ||
        data[2] != 1 || data[3] != 0) return false;
    stream = {ControlWire32(data + 4), ControlWire32(data + 8)};
    return stream.epoch && stream.epoch == worldEpoch && stream.generation;
}
} // namespace

bool BoneLog(unsigned& count)
{
    if (count >= 16) return false;
    ++count;
    return true;
}

bool SendBoneControl(ENetPeer* peer, uint8_t type, std::vector<uint8_t> const& payload)
{
    std::vector<uint8_t> message;
    message.reserve(1 + payload.size());
    message.push_back(type);
    message.insert(message.end(), payload.begin(), payload.end());
    return Send(peer, message.data(), message.size(), ChannelDefinitions, true);
}

bool SaneNpc(NpcRecord const& record)
{
    if (!record.incarnation || record.hitPlayer >= MaxPlayers || record.guidePlayer >= MaxPlayers ||
        record.partyType > 8 || record.waiting > 1 || record.contact > 1 || record.attitude > 8 ||
        record.aiMode > 11 || record.attackReason > 21 || record.damageType > 7 ||
        record.damageAmount < -1000000 || record.damageAmount > 1000000 ||
        !std::isfinite(record.damageMultiplier) || std::fabs(record.damageMultiplier) > 1000.0f) return false;
    for (auto const& motion : record.motions) {
        if ((motion.active & ~3u) || (motion.active && !(motion.active & 1u)) ||
            !std::isfinite(motion.phase) || motion.phase < 0.0f || motion.phase > 1.0f ||
            !std::isfinite(motion.speed) || std::fabs(motion.speed) > 100.0f ||
            !std::isfinite(motion.weight) || motion.weight < 0.0f || motion.weight > 10.0f ||
            !std::isfinite(motion.intro) || motion.intro < 0.0f || motion.intro > 100000.0f ||
            !std::isfinite(motion.extro) || motion.extro < 0.0f || motion.extro > 100000.0f ||
            (motion.active && (!motion.name || !motion.instance))) return false;
    }
    for (float v : record.position) if (!std::isfinite(v) || std::fabs(v) > 1e7f) return false;
    float length = 0.0f;
    for (float v : record.rotation) {
        if (!std::isfinite(v) || std::fabs(v) > 2.0f) return false;
        length += v * v;
    }
    return length > 0.01f && record.hitPoints >= -1000000 && record.hitPoints <= 1000000 &&
           record.hitPointsMax >= 1 && record.hitPointsMax <= 1000000 && record.partyPlayer < MaxPlayers;
}

PreparedControl PrepareControl(NpcRecord const& record, bool forceBaseline)
{
    PreparedControl result;
    const npcwire::Stream stream{worldEpoch, controlGeneration};
    if (!stream.epoch || !stream.generation) return result;
    if (!controlSender.Matches(stream)) {
        controlSender = npcwire::Context{};
        controlStates.clear();
        controlKeys.clear();
        nextControlId = 0;
        nextControlOffer = 0;
        controlIdExhaustedLogged = false;
        for (uint8_t i = 0; i < MaxPlayers; ++i) ForgetControlPeer(i);
        if (!controlSender.Reset(stream.epoch, stream.generation)) return result;
    }
    npcwire::Control full;
    if (!npcwire::FromNpcRecord(record, 0, 0, true, full)) return result;
    const NpcKeyValue key = NpcKey(record.guid);
    auto found = controlStates.find(key);
    if (found == controlStates.end()) {
        if (nextControlId >= UINT16_MAX) {
            if (!controlIdExhaustedLogged) {
                controlIdExhaustedLogged = true;
                Log("net: NPC control IDs exhausted; using full-record fallback");
            }
            return result;
        }
        ControlState state;
        state.id = static_cast<uint16_t>(++nextControlId);
        if (!controlSender.AddId(stream, state.id, full.guid)) return result;
        found = controlStates.emplace(key, state).first;
        controlKeys.emplace(state.id, key);
    }
    auto& state = found->second;
    const uint8_t lifeFlags = record.flags & 0x83u;
    const bool lifeChanged = !state.current.id || state.current.incarnation != record.incarnation ||
                             state.lifeFlags != lifeFlags;
    npcwire::Control dynamic;
    if (!npcwire::FromNpcRecord(record, state.current.anchor, state.current.baseline, false, dynamic)) return result;
    npcwire::Anchor retainedAnchor;
    const bool baselineRetained = controlSender.GetAnchor(state.id, state.current.anchor, retainedAnchor);
    bool pendingSameLife = false;
    for (uint8_t peerId = 2; peerId < MaxPlayers; ++peerId) {
        const auto offered = controlOffered[peerId].find(state.id);
        if (offered == controlOffered[peerId].end() ||
            offered->second.ref.incarnation != state.current.incarnation ||
            !SameControlRef(offered->second.ref, state.current)) continue;
        const auto ack = controlAcknowledged[peerId].find(state.id);
        if (ack == controlAcknowledged[peerId].end() ||
            !SameControlRef(ack->second, state.current)) {
            pendingSameLife = true;
            break;
        }
    }
    const npcwire::control_policy::BaselinePin pin{
        {state.current.id, state.current.baseline, state.current.incarnation, state.current.anchor},
        state.lifeFlags,
        pendingSameLife
    };
    pendingSameLife = npcwire::control_policy::SameLifeDefinition(
        pin, pin.reference, record.incarnation, lifeFlags);
    if (pendingSameLife) ++npcControlPending;
    const bool needsBaseline = forceBaseline ||
        npcwire::control_policy::ShouldAllocateBaseline(
            state.current.id != 0, lifeChanged, baselineRetained,
            pendingSameLife, !baselineRetained);
    if (needsBaseline) {
        if (state.nextBaseline == UINT16_MAX || state.nextAnchor == UINT32_MAX) {
            if (BoneLog(controlErrors)) Logf("net: NPC control tokens exhausted for id %u", static_cast<unsigned>(state.id));
            return result;
        }
        const uint16_t token = ++state.nextBaseline;
        const uint32_t anchor = ++state.nextAnchor;
        if (!npcwire::FromNpcRecord(record, anchor, token, true, full) ||
            !controlSender.AdvanceLife(stream, state.id, record.incarnation) ||
            !controlSender.AddBaseline(stream, state.id, token, anchor, full)) {
            if (BoneLog(controlErrors)) Logf("net: NPC control baseline refused for id %u", static_cast<unsigned>(state.id));
            return result;
        }
        state.current = {state.id, token, record.incarnation, anchor};
        state.lifeFlags = record.flags & 0x83u;
        for (uint8_t peerId = 2; peerId < MaxPlayers; ++peerId)
            controlAcknowledged[peerId].erase(state.id);
        ++npcControlCreated;
        if (BoneLog(controlBaselineLogs))
            Logf("net: NPC control baseline id=%u token=%u anchor=%u",
                 static_cast<unsigned>(state.id), static_cast<unsigned>(token), anchor);
    }
    if (!npcwire::FromNpcRecord(record, state.current.anchor, state.current.baseline, false, dynamic)) return result;
    result.available = true;
    state.latest = record;
    result.ref = state.current;
    result.value = std::move(dynamic);
    result.encoded.clear();
    result.encodable = npcwire::EncodeRecord(controlSender, result.value, result.encoded);
    if (!result.encodable &&
        npcwire::FromNpcRecord(record, state.current.anchor, state.current.baseline, true, result.value)) {
        result.exact = true;
        result.encoded.clear();
        result.encodable = npcwire::EncodeRecord(controlSender, result.value, result.encoded, true);
    }
    return result;
}

bool ControlReady(uint8_t peerId, PreparedControl const& prepared)
{
    if (!prepared.available || !prepared.encodable) return false;
    const auto ack = controlAcknowledged[peerId].find(prepared.ref.id);
    const auto offered = controlOffered[peerId].find(prepared.ref.id);
    return ack != controlAcknowledged[peerId].end() &&
           SameControlRef(ack->second, prepared.ref) &&
           offered != controlOffered[peerId].end() &&
           npcwire::control_policy::AlreadySatisfied(
               ToPolicyOffer(offered->second), ToPolicyRef(prepared.ref), false);
}

bool ControlQueued(uint8_t peerId, PreparedControl const& prepared)
{
    const auto offer = controlOffered[peerId].find(prepared.ref.id);
    return prepared.available && prepared.encodable && offer != controlOffered[peerId].end() &&
           SameControlRef(offer->second.ref, prepared.ref) &&
           offer->second.idQueued && offer->second.baselineQueued;
}

bool OfferControl(uint8_t peerId, PreparedControl const& prepared, bool force)
{
    if (!prepared.available || !peers[peerId] || !clientInWorld[peerId]) return false;
    ControlOffer transaction;
    const auto offered = controlOffered[peerId].find(prepared.ref.id);
    if (offered != controlOffered[peerId].end()) transaction = offered->second;
    const auto ack = controlAcknowledged[peerId].find(prepared.ref.id);
    if (ack == controlAcknowledged[peerId].end() || !SameControlRef(ack->second, prepared.ref))
        transaction.acknowledged = false;
    const auto policyRef = ToPolicyRef(prepared.ref);
    auto policyOffer = ToPolicyOffer(transaction);
    if (npcwire::control_policy::AlreadySatisfied(policyOffer, policyRef, force)) return true;
    if (!npcwire::control_policy::BeginOffer(policyOffer, policyRef, force, nextControlOffer)) {
        if (BoneLog(controlErrors)) Log("net: NPC control offer serials exhausted; using full-record fallback");
        return false;
    }
    transaction.ref = prepared.ref;
    transaction.at = enet_time_get();
    transaction.serial = policyOffer.serial;
    transaction.idQueued = policyOffer.idQueued;
    transaction.baselineQueued = policyOffer.baselineQueued;
    transaction.acknowledged = policyOffer.acknowledged;
    if (!transaction.acknowledged) controlAcknowledged[peerId].erase(prepared.ref.id);
    std::vector<uint8_t> id, baseline;
    const bool sendId = npcwire::control_policy::ShouldSendId(policyOffer);
    const bool sendBaseline = npcwire::control_policy::ShouldSendBaseline(policyOffer);
    if (sendId && npcwire::EncodeId(controlSender, transaction.serial, prepared.ref.id, id) &&
        SendBoneControl(peers[peerId], MsgControlId, id))
        transaction.idQueued = true;
    if (sendBaseline && transaction.idQueued &&
        npcwire::EncodeBaseline(controlSender, transaction.serial, prepared.ref.id,
                                prepared.ref.baseline, baseline) &&
        SendBoneControl(peers[peerId], MsgControlBaseline, baseline))
        transaction.baselineQueued = true;
    controlOffered[peerId][prepared.ref.id] = transaction;
    if (!transaction.idQueued || !transaction.baselineQueued) {
        ++npcControlPartial;
        if (BoneLog(controlErrors))
            Logf("net: could not offer NPC control id=%u to player=%u",
                 static_cast<unsigned>(prepared.ref.id), static_cast<unsigned>(peerId));
        return false;
    }
    return true;
}

namespace {
void RequestControl(ENetPeer* peer, npcwire::Stream stream, uint16_t id, uint32_t anchor, uint16_t baseline,
                    npcwire::ResyncReason reason)
{
    if (!id || !stream.epoch || !stream.generation || stream.epoch != worldEpoch) return;
    const RequestedControlEntry entry{stream.generation, anchor, baseline, reason};
    if (const auto existing = requestedControls.find(id);
        existing != requestedControls.end() && existing->second == entry) return;
    npcwire::Context future;
    npcwire::Context const* context = &controlReceiver;
    if (!controlReceiver.Matches(stream)) {
        if (!future.Reset(stream.epoch, stream.generation)) return;
        context = &future;
    }
    npcwire::Signal signal;
    signal.kind = npcwire::SignalKind::Resync;
    signal.reference.id = id;
    signal.reference.anchor = anchor;
    signal.reference.baseline = baseline;
    signal.reason = reason;
    std::vector<uint8_t> message;
    if (npcwire::EncodeSignal(*context, npcSequence, signal, message) &&
        SendBoneControl(peer, MsgControlSignal, message)) {
        requestedControls[id] = entry;
        if (BoneLog(controlRequestLogs))
            Logf("net: requested NPC control id=%u reason=%u", static_cast<unsigned>(id), static_cast<unsigned>(reason));
    }
}

} // namespace

bool DecodeControls(ENetPeer* peer, NpcsHeader const& header, uint8_t const* data, size_t size,
                    std::vector<NpcRecord>& records, std::vector<uint8_t>& acceptedBatch,
                    bool retainStaleAnchor)
{
    npcwire::Stream stream;
    if (!ControlStream(data, size, 'C', stream) || stream.generation < receivedControlGeneration ||
        size > npcwire::MaxBatchBytes || BoneWire16(data + 16) != header.count ||
        ControlWire32(data + 12) != header.publication) return false;
    size_t offset = sizeof(npcwire::WireBatchHeader);
    std::unordered_set<uint16_t> seen;
    std::vector<NpcRecord> result;
    std::vector<uint8_t> batch(data, data + sizeof(npcwire::WireBatchHeader));
    for (unsigned i = 0; i < header.count; ++i) {
        if (size - offset < sizeof(npcwire::WireControl)) return false;
        uint8_t const* raw = data + offset;
        const uint16_t id = BoneWire16(raw);
        const uint32_t anchor = ControlWire32(raw + offsetof(npcwire::WireControl, anchor));
        const uint16_t baseline = BoneWire16(raw + offsetof(npcwire::WireControl, baseline));
        size_t length = 0;
        if (!id || !anchor || !baseline || !seen.insert(id).second ||
            !npcwire::RecordBytes(raw, size - offset, length)) return false;
        npcwire::Guid guid;
        if (!controlReceiver.Matches(stream) || !controlReceiver.FindGuid(id, guid)) {
            RequestControl(peer, stream, id, anchor, baseline, npcwire::ResyncReason::UnknownId);
            return false;
        }
        const auto previous = npcPublications.find(NpcKey(guid.data()));
        if (!retainStaleAnchor && previous != npcPublications.end() && previous->second.publication.initialized &&
            !npcwire::Newer(header.publication, previous->second.publication.latest)) {
            ++npcStale;
            ++npcControlStale;
            offset += length;
            continue;
        }
        npcwire::Control value;
        if (!npcwire::DecodeRecord(controlReceiver, raw, length, value)) {
            npcwire::Anchor saved;
            RequestControl(peer, stream, id, anchor, baseline,
                           controlReceiver.GetAnchor(id, anchor, saved) ? npcwire::ResyncReason::UnknownBaseline
                                                                       : npcwire::ResyncReason::UnknownAnchor);
            return false;
        }
        NpcRecord record;
        if (!npcwire::ToNpcRecord(value, record) || !SaneNpc(record)) return false;
        result.push_back(record);
        batch.insert(batch.end(), raw, raw + length);
        offset += length;
    }
    if (offset != size) return false;
    if (result.empty()) {
        records.clear();
        acceptedBatch.clear();
        return header.count != 0;
    }
    batch[16] = static_cast<uint8_t>(result.size());
    batch[17] = static_cast<uint8_t>(result.size() >> 8);
    records = std::move(result);
    acceptedBatch = std::move(batch);
    return true;
}

namespace {
npcwire::control_policy::ResyncReason ToPolicyResyncReason(npcwire::ResyncReason reason)
{
    switch (reason) {
    case npcwire::ResyncReason::UnknownId: return npcwire::control_policy::ResyncReason::UnknownId;
    case npcwire::ResyncReason::UnknownBaseline: return npcwire::control_policy::ResyncReason::UnknownBaseline;
    case npcwire::ResyncReason::UnknownAnchor: return npcwire::control_policy::ResyncReason::UnknownAnchor;
    case npcwire::ResyncReason::Invalid: return npcwire::control_policy::ResyncReason::Invalid;
    default: return npcwire::control_policy::ResyncReason::None;
    }
}

bool SendUnreliableNpc(ENetPeer* peer, void const* data, size_t size)
{
    if (peer->channels[ChannelNpcs].outgoingUnreliableSequenceNumber == UINT16_MAX &&
        !Send(peer, nullptr, 0, ChannelNpcs, true)) {
        if (BoneLog(controlErrors)) Log("net: NPC sequence checkpoint could not be queued");
        return false;
    }
    return Send(peer, data, size, ChannelNpcs, false);
}

bool SendEssentialNpc(uint8_t peerId, NpcRecord const& record, PreparedControl const& prepared)
{
    if (ControlQueued(peerId, prepared)) {
        npcwire::Control exact;
        std::vector<uint8_t> batch;
        const NpcsHeader header{MsgControlBatch, ++npcSequence, enet_time_get(), worldEpoch,
                                1, ++npcPublication, 1, 1};
        if (npcwire::FromNpcRecord(record, prepared.ref.anchor, prepared.ref.baseline, true, exact) &&
            npcwire::EncodeBatch(controlSender, header.publication, {exact}, batch, true)) {
            std::vector<uint8_t> message(sizeof(header));
            std::memcpy(message.data(), &header, sizeof(header));
            message.insert(message.end(), batch.begin(), batch.end());
            if (Send(peers[peerId], message.data(), message.size(), ChannelDefinitions, true)) return true;
            if (BoneLog(controlErrors)) Log("net: exact reliable NPC control send failed; using full record");
        } else if (BoneLog(controlErrors)) {
            Log("net: exact reliable NPC control encode failed; using full record");
        }
    }
    return SendFullNpc(peers[peerId], record, true);
}

struct PendingControls {
    std::vector<npcwire::Control> values;
    std::vector<NpcRecord> originals;
    std::vector<uint16_t> ids;
    size_t bytes = sizeof(NpcsHeader) + sizeof(npcwire::WireBatchHeader);
    bool exact = false;
};

bool FlushControls(uint8_t id, PendingControls& pending)
{
    if (pending.values.empty()) return false;
    const NpcsHeader header{MsgControlBatch, ++npcSequence, enet_time_get(), worldEpoch, 1, ++npcPublication,
                            static_cast<uint8_t>(pending.values.size()), 1};
    bool queued = false;
    std::vector<uint8_t> batch;
    if (npcwire::EncodeBatch(controlSender, header.publication, pending.values, batch, pending.exact) &&
        sizeof(header) + batch.size() <= NpcDatagramBudget) {
        std::vector<uint8_t> message(sizeof(header));
        std::memcpy(message.data(), &header, sizeof(header));
        message.insert(message.end(), batch.begin(), batch.end());
        if (BoneLog(controlBatchLogs))
            Logf("net: player %u compact NPC records=%u record_bytes=%u datagram_bytes=%u",
                 static_cast<unsigned>(id), static_cast<unsigned>(pending.values.size()),
                 static_cast<unsigned>(batch.size() - sizeof(npcwire::WireBatchHeader)),
                 static_cast<unsigned>(message.size()));
        queued = SendUnreliableNpc(peers[id], message.data(), message.size());
        if (!queued && BoneLog(controlErrors))
            Log("net: compact NPC batch could not be sent");
    } else {
        if (BoneLog(controlErrors)) Log("net: compact NPC batch failed; using full-record fallback");
        for (auto const& record : pending.originals) queued = SendFullNpc(peers[id], record, false) || queued;
    }
    pending = PendingControls{};
    return queued;
}

} // namespace

bool SendFullNpc(ENetPeer* peer, NpcRecord const& record, bool reliable)
{
    ++npcControlFallback;
    uint8_t buffer[sizeof(NpcsHeader) + sizeof(NpcRecord)];
    const NpcsHeader header{MsgNpcs, ++npcSequence, enet_time_get(), worldEpoch, 1, ++npcPublication, 1, 1};
    std::memcpy(buffer, &header, sizeof(header));
    std::memcpy(buffer + sizeof(header), &record, sizeof(record));
    const bool sent = reliable ? Send(peer, buffer, sizeof(buffer), ChannelDefinitions, true)
                               : SendUnreliableNpc(peer, buffer, sizeof(buffer));
    if (!sent && BoneLog(controlErrors))
        Log("net: full-record NPC fallback could not be sent");
    return sent;
}

void ForgetControlPeer(uint8_t id)
{
    controlOffered[id].clear();
    controlAcknowledged[id].clear();
}

void ClearClientControlTables()
{
    controlReceiver = npcwire::Context{};
    receivedControlGeneration = 0;
    requestedControls.clear();
}

void ResetNpcCountersOnStart()
{
    worldEpoch = npcPublication = npcSequence = 0;
}

void HandleControlId(ENetPeer* peer, ENetPacket const* packet)
{
    if (isHost || peer != server || !(packet->flags & ENET_PACKET_FLAG_RELIABLE)) { ++npcRejected; return; }
    auto const* data = packet->data + 1;
    const size_t size = packet->dataLength - 1;
    npcwire::Stream stream;
    if (!ControlStream(data, size, 'I', stream) || stream.generation < receivedControlGeneration) {
        ++npcRejected;
        return;
    }
    uint16_t id = 0;
    if (stream.generation > receivedControlGeneration) {
        npcwire::Context candidate;
        if (!candidate.Reset(stream.epoch, stream.generation) || !npcwire::DecodeId(candidate, data, size, id)) {
            ++npcRejected;
            return;
        }
        controlReceiver = std::move(candidate);
        receivedControlGeneration = stream.generation;
        requestedControls.clear();
    } else if (!npcwire::DecodeId(controlReceiver, data, size, id)) {
        ++npcRejected;
    }
}

namespace {
bool OnNpcBaselineAck(uint8_t peerId, npcwire::Signal const& signal)
{
    const auto offered = controlOffered[peerId].find(signal.reference.id);
    const auto key = controlKeys.find(signal.reference.id);
    const auto state = key == controlKeys.end() ? controlStates.end() : controlStates.find(key->second);
    if (offered == controlOffered[peerId].end() || state == controlStates.end() ||
        !SameControlRef(state->second.current, signal.reference)) {
        ++npcRejected;
        return false;
    }
    auto policyOffer = ToPolicyOffer(offered->second);
    const auto policyRef = ToPolicyRef(signal.reference);
    const bool firstAck = !policyOffer.acknowledged;
    if (!npcwire::control_policy::CompleteAck(policyOffer, policyRef, signal.sequence)) {
        ++npcRejected;
        return false;
    }
    auto& entry = offered->second;
    entry.acknowledged = policyOffer.acknowledged;
    entry.resyncHandled = false;
    entry.resyncReason = npcwire::ResyncReason::None;
    entry.resyncReference = {};
    controlAcknowledged[peerId][signal.reference.id] = signal.reference;
    if (firstAck) {
        ++npcControlAckAccepted;
        if (BoneLog(controlRequestLogs))
            Logf("npc-control: ackaccepted id=%u baseline=%u pending=%u created=%u stale=%u resync=%u fallback=%u partial=%u",
                 static_cast<unsigned>(signal.reference.id), static_cast<unsigned>(signal.reference.baseline),
                 npcControlPending, npcControlCreated, npcControlStale, npcControlResync,
                 npcControlFallback, npcControlPartial);
    }
    return false;
}

bool OnNpcResync(uint8_t peerId, npcwire::Signal const& signal, uint8_t& peerIdOut,
                 NpcRecord& latestOut)
{
    if (!clientInWorld[peerId]) { ++npcRejected; return false; }
    const auto key = controlKeys.find(signal.reference.id);
    if (key == controlKeys.end()) { ++npcRejected; return false; }
    const auto state = controlStates.find(key->second);
    if (state == controlStates.end() || !state->second.current.id) { ++npcRejected; return false; }
    auto& transaction = controlOffered[peerId][signal.reference.id];
    const auto policyRef = ToPolicyRef(signal.reference);
    const auto policyReason = ToPolicyResyncReason(signal.reason);
    npcwire::control_policy::ResyncCursor cursor{
        ToPolicyRef(transaction.resyncReference),
        ToPolicyResyncReason(transaction.resyncReason),
        transaction.resyncHandled
    };
    if (npcwire::control_policy::DuplicateResync(cursor, policyRef, policyReason)) return false;
    controlAcknowledged[peerId].erase(signal.reference.id);
    transaction.acknowledged = false;
    npcwire::Anchor saved;
    const uint16_t baselineAge =
        static_cast<uint16_t>(state->second.nextBaseline - state->second.current.baseline);
    const bool evicted = !controlSender.GetAnchor(state->second.id, state->second.current.anchor, saved) ||
                         baselineAge >= npcwire::MaxBaselinesPerNpc ||
                         controlSender.RetainedBaselines(state->second.id) == 0;
    const NpcRecord latestCopy = state->second.latest;
    PreparedControl prepared = PrepareControl(latestCopy, evicted);
    const bool offerQueued = prepared.available && OfferControl(peerId, prepared, true);
    auto& updated = controlOffered[peerId][signal.reference.id];
    if (!offerQueued && SameControlRef(updated.ref, signal.reference) && updated.serial != 0 &&
        updated.idQueued && updated.baselineQueued) {
        updated.serial = 0;
        updated.idQueued = false;
        updated.baselineQueued = false;
        updated.acknowledged = false;
    }
    if (npcwire::control_policy::CommitResync(cursor, policyRef, policyReason, offerQueued)) {
        updated.resyncHandled = cursor.handled;
        updated.resyncReference = signal.reference;
        updated.resyncReason = signal.reason;
    } else {
        updated.resyncHandled = false;
        updated.resyncReason = npcwire::ResyncReason::None;
        updated.resyncReference = {};
    }
    ++npcControlResync;
    if (BoneLog(controlRequestLogs))
        Logf("npc-control: resync id=%u reason=%u evicted=%u pending=%u created=%u fallback=%u",
             static_cast<unsigned>(signal.reference.id), static_cast<unsigned>(signal.reason),
             evicted ? 1u : 0u, npcControlPending, npcControlCreated, npcControlFallback);
    peerIdOut = peerId;
    latestOut = latestCopy;
    return true;
}
} // namespace

bool HandleControlSignal(ENetPeer* peer, ENetPacket* packet, uint8_t& peerIdOut, NpcRecord& latestOut)
{
    if (!isHost || !(packet->flags & ENET_PACKET_FLAG_RELIABLE)) { ++npcRejected; return false; }
    const uint8_t peerId = PeerSlotOf(peer);
    if (!peerId) { ++npcRejected; return false; }
    npcwire::Signal signal;
    if (!npcwire::DecodeSignal(controlSender, packet->data + 1, packet->dataLength - 1, signal)) {
        ++npcRejected;
        return false;
    }
    if (signal.kind == npcwire::SignalKind::Ack) return OnNpcBaselineAck(peerId, signal);
    return OnNpcResync(peerId, signal, peerIdOut, latestOut);
}

bool HandleControlBaseline(ENetPeer* peer, ENetPacket const* packet)
{
    if (isHost || peer != server || !(packet->flags & ENET_PACKET_FLAG_RELIABLE)) { ++npcRejected; return true; }
    auto const* data = packet->data + 1;
    const size_t size = packet->dataLength - 1;
    npcwire::Stream stream;
    if (!ControlStream(data, size, 'B', stream) || stream.generation < receivedControlGeneration ||
        size != sizeof(npcwire::WireBatchHeader) + sizeof(npcwire::WireBaselinePrefix) + sizeof(npcwire::WireFullRecord)) {
        ++npcRejected;
        return true;
    }
    const uint16_t id = BoneWire16(data + sizeof(npcwire::WireBatchHeader));
    npcwire::Guid guid;
    if (!controlReceiver.Matches(stream) || !controlReceiver.FindGuid(id, guid)) {
        RequestControl(peer, stream, id, 0, 0, npcwire::ResyncReason::UnknownId);
        ++npcRejected;
        return true;
    }
    npcwire::BaselineRef ref;
    if (!npcwire::DecodeBaseline(controlReceiver, data, size, ref)) { ++npcRejected; return true; }
    requestedControls.erase(ref.id);
    npcwire::Signal ack;
    ack.reference = ref;
    std::vector<uint8_t> message;
    if (!npcwire::EncodeSignal(controlReceiver, ControlWire32(data + 12), ack, message) ||
        !SendBoneControl(peer, MsgControlSignal, message)) {
        if (BoneLog(controlErrors)) Log("net: could not acknowledge NPC control baseline");
    }
    return true;
}

} // namespace g3t::net::internal

namespace g3t::net {

void SetNpcHandlers(NpcHandler npcs, MotionNameHandler names)
{
    handlers.npc = npcs;
    handlers.motionName = names;
}

void SetNpcReleaseHandler(NpcReleaseHandler handler) { handlers.npcRelease = handler; }
void SetNpcRemovalHandler(NpcRemovalHandler handler) { handlers.npcRemoval = handler; }
void SetNpcEquipmentHandler(NpcEquipmentHandler handler) { handlers.npcEquipment = handler; }

void ResetNpcReplication()
{
    for (auto& origins : npcOrigins) origins.clear();
    npcPublications.clear();
    ResetBoneReplicationState();
    controlSender = npcwire::Context{};
    ClearClientControlTables();
    controlStates.clear();
    controlKeys.clear();
    nextControlId = 0;
    nextControlOffer = 0;
    controlIdExhaustedLogged = false;
    npcControlPending = npcControlCreated = npcControlAckAccepted = 0;
    npcControlStale = npcControlResync = npcControlFallback = npcControlPartial = 0;
    npcOrderedSetupPoses = npcOrderedSetupControls = npcAnchorsSent = npcAnchorsReceived = 0;
    for (uint8_t i = 0; i < MaxPlayers; ++i) ForgetControlPeer(i);
    if (isHost && !controlGenerationExhausted) {
        if (controlGeneration == UINT32_MAX) {
            controlGeneration = 0;
            controlGenerationExhausted = true;
        } else {
            ++controlGeneration;
        }
    }
}

void NpcReplicationCounts(unsigned& accepted, unsigned& rejected, unsigned& stale)
{
    accepted = npcAccepted;
    rejected = npcRejected;
    stale = npcStale;
}

bool SendNpcsTo(uint8_t to, NpcRecord const* records, int count, bool reliable)
{
    if (!host || !isHost || !records || count <= 0 || count > MaxNpcsPerMessage ||
        (to && (to < 2 || to >= MaxPlayers || !peers[to]))) return false;
    PendingControls pending[MaxPlayers];
    bool queued = false;
    for (int i = 0; i < count; ++i) {
        NpcRecord record = records[i];
        if (!record.sent) record.sent = enet_time_get();
        const auto prepared = PrepareControl(record);
        const bool encodable = prepared.encodable &&
            sizeof(NpcsHeader) + sizeof(npcwire::WireBatchHeader) + prepared.encoded.size() <= NpcDatagramBudget;
        for (uint8_t peerId = 2; peerId < MaxPlayers; ++peerId) {
            if (to && peerId != to) continue;
            if (!peers[peerId] || !clientInWorld[peerId]) continue;
            OfferControl(peerId, prepared);
            auto& batch = pending[peerId];
            if (reliable || !encodable || !ControlReady(peerId, prepared)) {
                FlushControls(peerId, batch);
                const bool orderedSetup = encodable && ControlQueued(peerId, prepared) && !ControlReady(peerId, prepared);
                const bool sent = (reliable || orderedSetup)
                                   ? SendEssentialNpc(peerId, record, prepared)
                                   : SendFullNpc(peers[peerId], record, false);
                if (sent && orderedSetup) ++npcOrderedSetupControls;
                queued = sent || queued;
                continue;
            }
            if (batch.bytes + prepared.encoded.size() > NpcDatagramBudget ||
                (!batch.values.empty() && batch.exact != prepared.exact) ||
                std::find(batch.ids.begin(), batch.ids.end(), prepared.ref.id) != batch.ids.end())
                queued = FlushControls(peerId, batch) || queued;
            batch.exact = prepared.exact;
            batch.bytes += prepared.encoded.size();
            batch.values.push_back(prepared.value);
            batch.originals.push_back(record);
            batch.ids.push_back(prepared.ref.id);
        }
    }
    for (uint8_t peerId = 2; peerId < MaxPlayers; ++peerId) {
        if (to && peerId != to) continue;
        if (peers[peerId]) queued = FlushControls(peerId, pending[peerId]) || queued;
    }
    return queued;
}

void SendNpcs(NpcRecord const* records, int count, bool reliable)
{
    SendNpcsTo(0, records, count, reliable);
}

bool SendNpcReleaseTo(uint8_t to, uint8_t const* guids, int count)
{
    if (!host || !isHost || !guids || count <= 0 || count > MaxNpcsPerMessage ||
        (to && (to < 2 || to >= MaxPlayers || !peers[to]))) return false;
    uint8_t buffer[sizeof(NpcReleaseHeader) + MaxNpcsPerMessage * 16];
    const NpcReleaseHeader header{MsgNpcRelease, ++npcSequence, worldEpoch, 1u,
                                  ++npcPublication, static_cast<uint8_t>(count), localId};
    std::memcpy(buffer, &header, sizeof(header));
    std::memcpy(buffer + sizeof(header), guids, count * 16u);
    const size_t size = sizeof(header) + count * 16u;
    bool queued = false;
    for (uint8_t id = 2; id < MaxPlayers; ++id) {
        if ((to && id != to) || !peers[id]) continue;
        if (Send(peers[id], buffer, size, ChannelDefinitions, true)) {
            queued = true;
            for (int i = 0; i < count; ++i) sentPoses[id].erase(NpcKey(guids + i * 16));
        }
    }
    return queued;
}

void SendNpcRelease(uint8_t const* guids, int count)
{
    SendNpcReleaseTo(0, guids, count);
}

bool SendNpcRemovalTo(uint8_t to, NpcRemovalNote const& note)
{
    if (!isHost || !host || to < 2 || to >= MaxPlayers || !peers[to] ||
        !PeerJoined(to) || !InWorld(to) || !note.epoch ||
        note.epoch != worldEpoch || !note.incarnation) return false;
    const NpcRemovalMsg message{MsgNpcRemoval, note};
    if (!Send(peers[to], &message, sizeof(message), ChannelDefinitions, true)) return false;
    sentPoses[to].erase(NpcKey(note.guid));
    return true;
}

bool SendNpcEquipmentTo(uint8_t to, NpcEquipmentNote const& note)
{
    if (!isHost || !host || to < 2 || to >= MaxPlayers || !peers[to] ||
        !PeerJoined(to) || !InWorld(to) || !note.incarnation) return false;
    const NpcEquipmentMsg message{MsgNpcEquipment, note};
    return Send(peers[to], &message, sizeof(message), ChannelReliable, true);
}

void SendMotionName(uint32_t hash, char const* name)
{
    if (!host || !isHost || !name) return;
    MotionNameMsg message{MsgMotionName, hash, {}};
    strncpy_s(message.name, name, _TRUNCATE);
    SendBroadcast(&message, sizeof(message), ChannelReliable, true);
}

} // namespace g3t::net
