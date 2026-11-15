// SPDX-License-Identifier: GPL-3.0-or-later
#include "net_internal.h"

#include "log.h"
#include "npclease_presentation.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace g3t::net::internal {

unsigned poseSent = 0, poseReceived = 0, poseSendFailed = 0;
bonewire::Dictionary boneLayouts;
std::unordered_map<NpcKeyValue, bonewire::PoseCache, NpcKeyHash> sentPoses[MaxPlayers], decodedPoses;
uint64_t boneGeneration = 0, receivedBoneGeneration = 0;
uint32_t nextBoneLayout = 0;
std::unordered_map<uint16_t, uint32_t> boneOffered[MaxPlayers];
std::unordered_set<uint16_t> boneAcknowledged[MaxPlayers];
bool boneExhaustionLogged = false;
unsigned boneInvalidLogs = 0, boneFallbackFailureLogs = 0;

namespace {
bool boneGenerationExhausted = false;
uint32_t requestedBoneEpoch = 0, requestedBoneAt = 0;
uint16_t requestedBoneLayout = 0;
unsigned boneLayoutLogs = 0, boneAckLogs = 0, boneFullPoseLogs = 0, boneCompactPoseLogs = 0;
unsigned boneRequestLogs = 0, boneDefinitionFailureLogs = 0;

bool ValidPoseBoneHashes(uint32_t const* values, int length) noexcept
{
    constexpr unsigned Slots = static_cast<unsigned>(pose::MaxBones) * 2u;
    uint32_t seen[Slots] = {};
    for (int i = 0; i < length; ++i) {
        const uint32_t hash = values[i];
        if (!hash) return false;
        unsigned slot = (hash * 2654435761u) & (Slots - 1u);
        while (seen[slot] && seen[slot] != hash) slot = (slot + 1u) & (Slots - 1u);
        if (seen[slot]) return false;
        seen[slot] = hash;
    }
    return true;
}

bool ValidPoseBones(pose::Bone const* bones, int length) noexcept
{
    uint32_t hashes[pose::MaxBones];
    for (int i = 0; i < length; ++i) hashes[i] = bones[i].name;
    return ValidPoseBoneHashes(hashes, length);
}

bool ResolvePoseLayoutId(uint32_t const* hashes, int count, uint16_t& layoutId)
{
    layoutId = 0;
    if (worldEpoch && boneGeneration &&
        boneLayouts.Reset(worldEpoch, boneGeneration) == bonewire::Status::Ok) {
        auto status = boneLayouts.FindLayout(hashes, count, layoutId);
        if (status == bonewire::Status::InvalidArgument) {
            if (BoneLog(boneInvalidLogs)) Log("net: rejected pose with zero or duplicate bone hashes");
            return false;
        }
        if (status == bonewire::Status::UnknownLayout) {
            if (nextBoneLayout < UINT16_MAX) {
                layoutId = static_cast<uint16_t>(++nextBoneLayout);
                status = boneLayouts.Register(layoutId, hashes, count);
                if (status == bonewire::Status::Ok && BoneLog(boneLayoutLogs))
                    Logf("net: new bone layout=%u bones=%d definition_bytes=%u compact_pose_bytes=%u full_pose_bytes=%u",
                         static_cast<unsigned>(layoutId), count, static_cast<unsigned>(1 + bonewire::HeaderBytes + bonewire::CountBytes + count * 4),
                         static_cast<unsigned>(sizeof(BonePosePrefix) + count * bonewire::TransformBytes),
                         static_cast<unsigned>(3 + count * sizeof(pose::Bone)));
            } else if (!boneExhaustionLogged) {
                boneExhaustionLogged = true;
                Log("net: bone layout IDs exhausted; new layouts use full-hash pose fallback");
            }
        }
        if (status != bonewire::Status::Ok) {
            if (status != bonewire::Status::UnknownLayout && BoneLog(boneInvalidLogs))
                Logf("net: bone layout unavailable (%u); using full-hash pose", static_cast<unsigned>(status));
            layoutId = 0;
        }
        return true;
    }
    if (!ValidPoseBoneHashes(hashes, count)) {
        if (BoneLog(boneInvalidLogs)) Log("net: rejected pose with zero or duplicate bone hashes");
        return false;
    }
    if (BoneLog(boneInvalidLogs))
        Log("net: bone dictionary has no epoch or generation; using full-hash pose");
    return true;
}

void RequestBoneLayout(ENetPeer* peer, uint16_t layoutId)
{
    const uint32_t now = enet_time_get();
    if (requestedBoneEpoch == worldEpoch && requestedBoneLayout == layoutId &&
        static_cast<uint32_t>(now - requestedBoneAt) < 1000) return;
    std::vector<uint8_t> request;
    if (bonewire::EncodeRequest(worldEpoch, layoutId, request) == bonewire::Status::Ok &&
        SendBoneControl(peer, MsgBoneRequest, request)) {
        requestedBoneEpoch = worldEpoch;
        requestedBoneLayout = layoutId;
        requestedBoneAt = now;
        if (BoneLog(boneRequestLogs))
            Logf("net: requested unknown bone layout %u epoch %u", static_cast<unsigned>(layoutId), worldEpoch);
    } else if (BoneLog(boneDefinitionFailureLogs)) {
        Logf("net: could not request bone layout %u epoch %u", static_cast<unsigned>(layoutId), worldEpoch);
    }
}

struct DecodedCompactPose {
    uint8_t poseFlags = 0;
    uint8_t intervalMs = 16;
    bool anchor = false;
    bool essential = false;
    bool key = true;
    uint16_t layoutId = 0;
    size_t wireBytes = 0;
    bonewire::PoseStats stats{};
};

bool DecodeCompactPoseSection(ENetPeer* peer, NpcRecord const& record, uint8_t const* section, size_t sectionBytes,
                              enet_uint32 packetFlags, std::vector<pose::Bone>& decodedBones, DecodedCompactPose& out,
                              bool logFailures)
{
    if (sectionBytes < 2 + bonewire::DeltaHeaderBytes + 1 || !worldEpoch) {
        ++npcRejected;
        if (logFailures && BoneLog(boneInvalidLogs)) Log("net: rejected truncated or invalid compact pose");
        return false;
    }
    const uint8_t wireFlags = section[0];
    out.anchor = (wireFlags & PoseAnchorFlag) != 0;
    out.essential = (wireFlags & PoseEssentialFlag) != 0;
    if ((out.anchor || out.essential) && !(packetFlags & ENET_PACKET_FLAG_RELIABLE)) {
        ++npcRejected;
        return false;
    }
    out.poseFlags = wireFlags & ~(PoseAnchorFlag | PoseEssentialFlag);
    out.intervalMs = section[1];
    if (out.poseFlags & ~pose::Facial) {
        ++npcRejected;
        if (logFailures && BoneLog(boneInvalidLogs))
            Logf("net: rejected compact pose flags=%u", static_cast<unsigned>(out.poseFlags));
        return false;
    }
    if (!npclease::DecodePoseIntervalWire(out.intervalMs)) {
        ++npcRejected;
        return false;
    }
    uint8_t const* data = section + 2;
    const size_t size = sectionBytes - 2;
    uint16_t layoutId = 0;
    const auto status = decodedPoses[NpcKey(record.guid)].Decode(boneLayouts, data, size,
        record.incarnation, out.poseFlags, decodedBones, layoutId, &out.stats);
    if (status == bonewire::Status::UnknownLayout) {
        RequestBoneLayout(peer, layoutId);
        ++npcRejected;
        return false;
    }
    if (status != bonewire::Status::Ok) {
        if (status == bonewire::Status::UnknownPose) {
            PoseAckMsg request{MsgPoseAck, worldEpoch, receivedBoneGeneration, {}, record.incarnation, 0};
            std::memcpy(request.guid, record.guid, 16);
            if (!Send(peer, &request, sizeof(request), ChannelState, true)) ++bandwidth.poseAckMisses;
            ++bandwidth.poseReferenceMisses;
        } else {
            ++bandwidth.poseDecodeFailures;
        }
        ++npcRejected;
        if (logFailures && BoneLog(boneInvalidLogs))
            Logf("net: invalid compact pose layout (%u)", static_cast<unsigned>(status));
        return false;
    }
    out.layoutId = layoutId;
    out.wireBytes = sectionBytes;
    out.key = bonewire::ReadPose32(data + 2) == 0;
    if (out.anchor && !out.key) {
        ++npcRejected;
        return false;
    }
    return true;
}
} // namespace

void ForgetBonePeer(uint8_t id)
{
    boneOffered[id].clear();
    boneAcknowledged[id].clear();
    sentPoses[id].clear();
}

void ResetBoneReplicationState()
{
    boneLayouts.Clear();
    receivedBoneGeneration = 0;
    requestedBoneEpoch = 0;
    requestedBoneLayout = 0;
    requestedBoneAt = 0;
    for (uint8_t i = 0; i < MaxPlayers; ++i) ForgetBonePeer(i);
    if (isHost && !boneGenerationExhausted) {
        if (boneGeneration == UINT64_MAX) {
            boneGeneration = 0;
            boneGenerationExhausted = true;
        } else {
            ++boneGeneration;
        }
    }
}

void ClearClientNpcTables()
{
    boneLayouts.Clear();
    decodedPoses.clear();
    receivedBoneGeneration = 0;
    requestedBoneEpoch = 0;
    controlReceiver = npcwire::Context{};
    receivedControlGeneration = 0;
}

bool HandleNpcStreamMessage(ENetPeer* peer, ENetPacket* packet, uint8_t type)
{
    if (type == MsgControlBaseline) return HandleControlBaseline(peer, packet);
    if (type == MsgBoneDictionary) {
        if (isHost || peer != server || !(packet->flags & ENET_PACKET_FLAG_RELIABLE) ||
            packet->dataLength < 1 + bonewire::HeaderBytes || !worldEpoch) {
            ++npcRejected;
            return true;
        }
        uint8_t const* data = packet->data + 1;
        const size_t size = packet->dataLength - 1;
        const uint64_t generation = BoneWire64(data + 8);
        if (!generation || generation < receivedBoneGeneration) {
            ++npcRejected;
            if (BoneLog(boneInvalidLogs)) Log("net: rejected stale or invalid bone dictionary generation");
            return true;
        }
        bonewire::Status status;
        if (generation > receivedBoneGeneration) {
            bonewire::Dictionary candidate;
            candidate.Reset(worldEpoch, generation);
            status = candidate.DecodeDictionary(data, size);
            if (status == bonewire::Status::Ok) {
                boneLayouts = std::move(candidate);
                decodedPoses.clear();
                receivedBoneGeneration = generation;
                requestedBoneEpoch = 0;
            }
        } else {
            status = boneLayouts.DecodeDictionary(data, size);
        }
        if (status != bonewire::Status::Ok) {
            ++npcRejected;
            if (BoneLog(boneInvalidLogs))
                Logf("net: bone dictionary rejected (%u)", static_cast<unsigned>(status));
            return true;
        }
        const uint16_t layoutId = BoneWire16(data + 16);
        std::vector<uint8_t> ack;
        if (boneLayouts.EncodeAck(layoutId, ack) != bonewire::Status::Ok ||
            !SendBoneControl(peer, MsgBoneAck, ack)) {
            if (BoneLog(boneDefinitionFailureLogs))
                Logf("net: could not acknowledge bone layout %u", static_cast<unsigned>(layoutId));
        }
        return true;
    }
    if (type == MsgPoseAck) {
        if (!isHost || !(packet->flags & ENET_PACKET_FLAG_RELIABLE) ||
            packet->dataLength != sizeof(PoseAckMsg)) {
            ++npcRejected;
            return true;
        }
        const uint8_t id = PeerSlotOf(peer);
        if (!id) { ++npcRejected; return true; }
        PoseAckMsg ack{};
        std::memcpy(&ack, packet->data, sizeof(ack));
        const auto found = sentPoses[id].find(NpcKey(ack.guid));
        if (ack.epoch != worldEpoch || ack.generation != boneGeneration || !ack.incarnation ||
            found == sentPoses[id].end() || !found->second.Ack(ack.token, ack.incarnation)) {
            ++bandwidth.poseAckMisses;
            return true;
        }
        ++bandwidth.poseAcks;
        return true;
    }
    if (type == MsgBoneAck || type == MsgBoneRequest) {
        if (!isHost || !(packet->flags & ENET_PACKET_FLAG_RELIABLE)) { ++npcRejected; return true; }
        const uint8_t id = PeerSlotOf(peer);
        if (!id) { ++npcRejected; return true; }
        if (type == MsgBoneAck) {
            uint16_t layoutId = 0;
            const auto status = boneLayouts.DecodeAck(packet->data + 1, packet->dataLength - 1, layoutId);
            if (status != bonewire::Status::Ok || !boneOffered[id].count(layoutId)) {
                ++npcRejected;
                if (BoneLog(boneInvalidLogs))
                    Logf("net: rejected bone acknowledgement (%u)", static_cast<unsigned>(status));
                return true;
            }
            if (boneAcknowledged[id].insert(layoutId).second && BoneLog(boneAckLogs))
                Logf("net: player %u acknowledged bone layout %u", static_cast<unsigned>(id), static_cast<unsigned>(layoutId));
        } else {
            std::vector<uint8_t> definition;
            const auto status = boneLayouts.RespondToRequest(packet->data + 1, packet->dataLength - 1, definition);
            if (status != bonewire::Status::Ok) {
                ++npcRejected;
                if (BoneLog(boneInvalidLogs))
                    Logf("net: rejected missing or invalid bone request (%u)", static_cast<unsigned>(status));
                return true;
            }
            const uint16_t layoutId = BoneWire16(packet->data + 1 + 4);
            if (SendBoneControl(peer, MsgBoneDictionary, definition)) {
                boneOffered[id][layoutId] = enet_time_get();
            } else if (BoneLog(boneDefinitionFailureLogs)) {
                Logf("net: could not resend bone layout %u to player %u", static_cast<unsigned>(layoutId), static_cast<unsigned>(id));
            }
        }
        return true;
    }
    if (type == MsgNpcs || type == MsgPosedNpc || type == MsgCompactPosedNpc ||
        type == MsgControlBatch || type == MsgControlPose) {
        if (packet->dataLength < sizeof(NpcsHeader)) return true;
        if (isHost || peer != server) { ++npcRejected; return true; }
        NpcsHeader header{};
        std::memcpy(&header, packet->data, sizeof(header));
        if (!header.count || header.count > MaxNpcsPerMessage || header.epoch != worldEpoch || header.owner != 1) {
            ++npcRejected;
            return true;
        }
        uint16_t boneCount = 0;
        uint8_t poseFlags = 0;
        bool poseAnchor = false;
        bool poseEssential = (packet->flags & ENET_PACKET_FLAG_RELIABLE) != 0;
        uint8_t intervalMs = 16;
        uint16_t poseLayout = 0;
        size_t receivedPoseBytes = 0;
        bool receivedPoseKey = true;
        bonewire::PoseStats receivedPoseStats{};
        const pose::Bone* bones = nullptr;
        std::vector<pose::Bone> decodedBones;
        std::vector<NpcRecord> controlRecords;
        std::vector<uint8_t> acceptedBatch;
        const size_t recordsSize = sizeof(header) + header.count * sizeof(NpcRecord);
        if (type == MsgControlBatch || type == MsgControlPose) {
            size_t offset = sizeof(header);
            size_t controlSize = 0;
            if (type == MsgControlPose) {
                if (header.count != 1 || packet->dataLength < offset + sizeof(uint16_t)) { ++npcRejected; return true; }
                controlSize = BoneWire16(packet->data + offset);
                offset += sizeof(uint16_t);
                if (controlSize > packet->dataLength - offset) { ++npcRejected; return true; }
            } else {
                if (packet->dataLength > NpcDatagramBudget) { ++npcRejected; return true; }
                controlSize = packet->dataLength - offset;
            }
            uint8_t const* controlData = packet->data + offset;
            std::vector<uint8_t> expandedControl;
            if (type == MsgControlPose) {
                if (packet->dataLength < offset + controlSize + 2 + bonewire::DeltaHeaderBytes + 1) {
                    ++npcRejected;
                    return true;
                }
                poseAnchor = (packet->data[offset + controlSize] & PoseAnchorFlag) != 0;
                poseEssential = (packet->data[offset + controlSize] & PoseEssentialFlag) != 0;
                if ((poseAnchor || poseEssential) && !(packet->flags & ENET_PACKET_FLAG_RELIABLE)) { ++npcRejected; return true; }
                if (controlSize < 4 + sizeof(npcwire::WireControl)) { ++npcRejected; return true; }
                expandedControl.resize(sizeof(npcwire::WireBatchHeader));
                std::memcpy(expandedControl.data(), "NC\1\0", 4);
                std::memcpy(expandedControl.data() + 4, &header.epoch, 4);
                std::memcpy(expandedControl.data() + 8, controlData, 4);
                std::memcpy(expandedControl.data() + 12, &header.publication, 4);
                expandedControl[16] = header.count;
                expandedControl.insert(expandedControl.end(), controlData + 4, controlData + controlSize);
            }
            if (!DecodeControls(peer, header,
                                expandedControl.empty() ? controlData : expandedControl.data(),
                                expandedControl.empty() ? controlSize : expandedControl.size(),
                                controlRecords, acceptedBatch, poseAnchor || poseEssential)) {
                ++npcRejected;
                if (BoneLog(controlErrors)) Log("net: compact NPC control decode failed");
                return true;
            }
            if (controlRecords.empty()) return true;
            offset += controlSize;
            if (type == MsgControlPose) {
                DecodedCompactPose compact{};
                if (!DecodeCompactPoseSection(peer, controlRecords.front(), packet->data + offset,
                                              packet->dataLength - offset, packet->flags, decodedBones, compact, false))
                    return true;
                poseFlags = compact.poseFlags;
                intervalMs = compact.intervalMs;
                poseAnchor = compact.anchor;
                poseEssential = compact.essential;
                poseLayout = compact.layoutId;
                receivedPoseBytes = packet->dataLength - sizeof(header) - 2 - controlSize;
                receivedPoseKey = compact.key;
                receivedPoseStats = compact.stats;
                bones = decodedBones.data();
                boneCount = static_cast<uint16_t>(decodedBones.size());
            }
            std::vector<npcwire::Control> acceptedControls;
            bool skipSequenceAdvance = poseAnchor;
            if (!skipSequenceAdvance && poseEssential && !controlRecords.empty()) {
                const auto pub = npcPublications.find(NpcKey(controlRecords.front().guid));
                skipSequenceAdvance = pub != npcPublications.end() && pub->second.publication.initialized &&
                    !npcwire::Newer(header.publication, pub->second.publication.latest);
            }
            if (!skipSequenceAdvance &&
                !npcwire::DecodeBatch(controlReceiver, acceptedBatch.data(), acceptedBatch.size(), acceptedControls)) {
                ++npcRejected;
                return true;
            }
        } else if (type == MsgPosedNpc) {
            if (header.count != 1 || packet->dataLength < recordsSize + sizeof(boneCount) + 2) { ++npcRejected; return true; }
            std::memcpy(&boneCount, packet->data + recordsSize, sizeof(boneCount));
            std::memcpy(&poseFlags, packet->data + recordsSize + sizeof(boneCount), sizeof(poseFlags));
            intervalMs = packet->data[recordsSize + sizeof(boneCount) + 1];
            if (!boneCount || boneCount > pose::MaxBones ||
                (poseFlags & ~pose::Facial) ||
                packet->dataLength != recordsSize + sizeof(boneCount) + 2 + boneCount * sizeof(pose::Bone)) {
                ++npcRejected;
                return true;
            }
            bones = reinterpret_cast<pose::Bone const*>(packet->data + recordsSize + sizeof(boneCount) + 2);
            receivedPoseBytes = packet->dataLength - recordsSize;
            if (!ValidPoseBones(bones, boneCount) || !npclease::DecodePoseIntervalWire(intervalMs)) {
                ++npcRejected;
                return true;
            }
        } else if (type == MsgCompactPosedNpc) {
            if (header.count != 1 || packet->dataLength < recordsSize + 2 + bonewire::DeltaHeaderBytes + 1) {
                ++npcRejected;
                if (BoneLog(boneInvalidLogs)) Log("net: rejected truncated or invalid compact pose");
                return true;
            }
            NpcRecord posedRecord{};
            std::memcpy(&posedRecord, packet->data + sizeof(header), sizeof(posedRecord));
            if (!SaneNpc(posedRecord)) { ++npcRejected; return true; }
            DecodedCompactPose compact{};
            if (!DecodeCompactPoseSection(peer, posedRecord, packet->data + recordsSize,
                                          packet->dataLength - recordsSize, packet->flags, decodedBones, compact, true))
                return true;
            poseFlags = compact.poseFlags;
            intervalMs = compact.intervalMs;
            poseAnchor = compact.anchor;
            poseEssential = compact.essential;
            poseLayout = compact.layoutId;
            receivedPoseBytes = compact.wireBytes;
            receivedPoseKey = compact.key;
            receivedPoseStats = compact.stats;
            bones = decodedBones.data();
            boneCount = static_cast<uint16_t>(decodedBones.size());
        } else if (packet->dataLength != recordsSize) {
            ++npcRejected;
            return true;
        }
        auto processRecords = [&](NpcRecord* records, int recordCount) {
            int count = 0;
            for (int i = 0; i < recordCount; ++i) {
                NpcRecord const& record = records[i];
                if (!SaneNpc(record)) { ++npcRejected; continue; }
                const NpcKeyValue key = NpcKey(record.guid);
                if (bones && poseLayout && poseAnchor) {
                    if (!decodedPoses[key].Store(header.publication, record.incarnation, poseLayout, poseFlags,
                                                 bones, boneCount, enet_time_get(), true, true)) {
                        ++bandwidth.poseDecodeFailures;
                        ++npcRejected;
                        continue;
                    }
                    ++npcAnchorsReceived;
                    PoseAckMsg ack{MsgPoseAck, worldEpoch, receivedBoneGeneration, {}, record.incarnation, header.publication};
                    std::memcpy(ack.guid, record.guid, 16);
                    if (!Send(peer, &ack, sizeof(ack), ChannelState, true)) ++bandwidth.poseAckMisses;
                    else ++bandwidth.poseAcks;
                }
                const bool newer = npcPublications[key].Snapshot(header.owner, header.publication);
                if (!newer) {
                    if (poseEssential && bones && handlers.npcPose && record.contact)
                        handlers.npcPose(record, bones, boneCount, poseFlags, true, intervalMs);
                    ++npcStale;
                    continue;
                }
                if (bones && poseLayout && !poseAnchor)
                    decodedPoses[key].Store(header.publication, record.incarnation, poseLayout, poseFlags,
                                            bones, boneCount, enet_time_get(), false);
                if (bones) {
                    const uint32_t decodedInt = npclease::DecodePoseIntervalWire(intervalMs);
                    const uint8_t statsTierInterval = static_cast<uint8_t>((std::min)(decodedInt, 100u));
                    bandwidth.Pose(statsTierInterval, packet->dataLength, receivedPoseBytes, boneCount, receivedPoseKey);
                    if (!poseLayout) {
                        receivedPoseStats.headerBytes = 2 + boneCount * sizeof(uint32_t);
                        receivedPoseStats.rotationBits = boneCount * 144;
                        receivedPoseStats.translationBits = boneCount * 48;
                        receivedPoseStats.includedBones = boneCount;
                    }
                    bandwidth.PoseComponents(receivedPoseStats.headerBytes + 2, receivedPoseStats.maskBits,
                        receivedPoseStats.rotationBits, receivedPoseStats.translationBits, receivedPoseStats.includedBones,
                        receivedPoseKey, receivedPoseStats.predictive, packet->dataLength - receivedPoseBytes - sizeof(header),
                        sizeof(header), receivedPoseStats.rigidBones, 0, receivedPoseStats.PaddingBits());
                }
                if (bones && handlers.npcPose)
                    handlers.npcPose(record, bones, boneCount, poseFlags, poseEssential, intervalMs);
                records[count++] = record;
            }
            return count;
        };
        auto finishRecords = [&](NpcRecord* records, int count) {
            if (!count) return;
            const int accepted = handlers.npc ? handlers.npc(records, count, enet_time_get(), header.sent, header.owner, header.generation) : 0;
            if (accepted < 0 || accepted > count) { Log("net: invalid accepted NPC count; publication refused"); return; }
            npcRejected += count - accepted;
            npcAccepted += accepted;
        };
        if (type == MsgControlBatch || type == MsgControlPose) {
            finishRecords(controlRecords.data(), processRecords(controlRecords.data(), static_cast<int>(controlRecords.size())));
        } else if (type == MsgPosedNpc || type == MsgCompactPosedNpc) {
            NpcRecord record{};
            std::memcpy(&record, packet->data + sizeof(header), sizeof(record));
            finishRecords(&record, processRecords(&record, 1));
        } else {
            NpcRecord batchRecords[MaxNpcsPerMessage];
            std::memcpy(batchRecords, packet->data + sizeof(header), header.count * sizeof(NpcRecord));
            finishRecords(batchRecords, processRecords(batchRecords, header.count));
        }
        return true;
    }
    if (type == MsgNpcRemoval) {
        if (isHost || peer != server || !(packet->flags & ENET_PACKET_FLAG_RELIABLE) ||
            packet->dataLength != sizeof(NpcRemovalMsg)) {
            ++npcRejected;
            Log("net: invalid NPC removal sender or message");
            return true;
        }
        NpcRemovalMsg message{};
        std::memcpy(&message, packet->data, sizeof(message));
        if (message.note.epoch != worldEpoch || !message.note.incarnation) { ++npcStale; return true; }
        decodedPoses.erase(NpcKey(message.note.guid));
        if (handlers.npcRemoval) handlers.npcRemoval(message.note);
        return true;
    }
    if (type == MsgNpcRelease) {
        if (packet->dataLength < sizeof(NpcReleaseHeader)) return true;
        if (isHost || peer != server) { ++npcRejected; return true; }
        NpcReleaseHeader header{};
        std::memcpy(&header, packet->data, sizeof(header));
        if (!header.count || header.count > MaxNpcsPerMessage ||
            packet->dataLength != sizeof(header) + header.count * 16u || header.epoch != worldEpoch ||
            header.owner != 1) {
            ++npcRejected;
            return true;
        }
        uint8_t guids[MaxNpcsPerMessage * 16];
        int count = 0;
        for (int i = 0; i < header.count; ++i) {
            uint8_t const* guid = packet->data + sizeof(header) + i * 16;
            const NpcKeyValue key = NpcKey(guid);
            if (!npcPublications[key].Release(header.owner, header.publication)) { ++npcStale; continue; }
            decodedPoses.erase(key);
            std::memcpy(guids + count++ * 16, guid, 16);
        }
        const int accepted = count && handlers.npcRelease ? handlers.npcRelease(header.owner, header.generation, guids, count) : 0;
        if (accepted < 0 || accepted > count) { Log("net: invalid accepted release count; publication refused"); return true; }
        npcRejected += count - accepted;
        return true;
    }
    if (type == MsgMotionName) {
        if (packet->dataLength != sizeof(MotionNameMsg)) return true;
        if (isHost) { Log("net: client NPC motion dictionary publication refused"); return true; }
        if (peer != server) { ++npcRejected; return true; }
        MotionNameMsg message{};
        std::memcpy(&message, packet->data, sizeof(message));
        message.name[sizeof(message.name) - 1] = '\0';
        if (message.hash != HashName(message.name)) { Log("net: NPC motion dictionary hash mismatch"); return true; }
        if (handlers.motionName) handlers.motionName(message.hash, message.name);
        return true;
    }
    return false;
}

} // namespace g3t::net::internal

namespace g3t::net {

void SetNpcPoseHandler(NpcPoseHandler handler) { handlers.npcPose = handler; }

void NpcPoseCounts(unsigned& sent, unsigned& received, unsigned& failed)
{
    sent = poseSent;
    received = poseReceived;
    failed = poseSendFailed;
}

bool SendPosedNpcTo(uint8_t to, NpcRecord const& record, pose::Bone const* bones, int count, uint8_t poseFlags, bool reliable,
                    uint8_t intervalMs)
{
    if (!host || !isHost || !bones || count <= 0 || count > pose::MaxBones || (poseFlags & ~pose::Facial) ||
        !npclease::DecodePoseIntervalWire(intervalMs) ||
        (to && (to < 2 || to >= MaxPlayers || !peers[to]))) return false;

    uint32_t hashes[pose::MaxBones];
    for (int i = 0; i < count; ++i) hashes[i] = bones[i].name;

    uint16_t layoutId = 0;
    if (!ResolvePoseLayoutId(hashes, count, layoutId)) return false;

    NpcsHeader header{MsgPosedNpc, 0, enet_time_get(), worldEpoch, 1, 0, 1, 1};
    const auto preparedControl = PrepareControl(record);
    const uint16_t boneCount = static_cast<uint16_t>(count);
    const size_t fullSize = sizeof(header) + sizeof(record) + sizeof(boneCount) + 2 + count * sizeof(*bones);
    std::vector<uint8_t> fallbackBuffer, definition, compact, compactMessage, controlPoseMessage, batch;
    std::vector<pose::Bone> reconstructed;
    std::vector<npcwire::Control> singleControl(1);
    bool definitionAttempted = false, definitionReady = false;
    bool queued = false;
    const NpcKeyValue npcKey = NpcKey(record.guid);

    for (uint8_t i = 2; i < MaxPlayers; ++i) {
        if (to && i != to) continue;
        if (!peers[i] || !clientInWorld[i]) continue;
        header.sequence = ++npcSequence;
        header.publication = ++npcPublication;
        if (!header.publication) header.publication = ++npcPublication;
        OfferControl(i, preparedControl);
        if (layoutId && !boneAcknowledged[i].count(layoutId)) {
            const auto sent = boneOffered[i].find(layoutId);
            const uint32_t now = enet_time_get();
            if (sent == boneOffered[i].end()) {
                if (!definitionAttempted) {
                    definitionAttempted = true;
                    definitionReady = boneLayouts.EncodeDictionary(layoutId, definition) == bonewire::Status::Ok;
                }
                if (definitionReady && SendBoneControl(peers[i], MsgBoneDictionary, definition)) {
                    boneOffered[i][layoutId] = now;
                } else if (BoneLog(boneDefinitionFailureLogs)) {
                    Logf("net: could not send bone layout %u to player %u; using full-hash pose",
                         static_cast<unsigned>(layoutId), static_cast<unsigned>(i));
                }
            }
        }
        const bool dictionaryReady = layoutId && boneAcknowledged[i].count(layoutId);
        bool compactReady = layoutId && (dictionaryReady || boneOffered[i].count(layoutId));
        bool poseKey = true;
        bonewire::PoseStats poseStats{};
        compactMessage.clear();
        controlPoseMessage.clear();
        auto& poseCache = sentPoses[i][npcKey];
        const bool anchor = compactReady &&
            poseCache.AnchorDue(record.sent, record.incarnation, layoutId, poseFlags);
        if (compactReady) {
            const auto status = poseCache.EncodeCompact(boneLayouts, layoutId, record.incarnation, poseFlags, bones, count,
                                                        record.sent, compact, poseKey, reconstructed, &poseStats, anchor);
            if (status == bonewire::Status::Ok) {
                compactMessage.resize(sizeof(header) + sizeof(record) + 2 + compact.size());
                NpcsHeader compactHeader = header;
                compactHeader.type = MsgCompactPosedNpc;
                std::memcpy(compactMessage.data(), &compactHeader, sizeof(compactHeader));
                std::memcpy(compactMessage.data() + sizeof(header), &record, sizeof(record));
                compactMessage[sizeof(header) + sizeof(record)] =
                    static_cast<uint8_t>(poseFlags | (anchor ? PoseAnchorFlag : 0) |
                                         (reliable ? PoseEssentialFlag : 0));
                compactMessage[sizeof(header) + sizeof(record) + 1] = intervalMs;
                std::memcpy(compactMessage.data() + sizeof(header) + sizeof(record) + 2, compact.data(), compact.size());
            } else if (BoneLog(boneInvalidLogs)) {
                Logf("net: could not encode bone layout %u (%u); using full-hash pose",
                     static_cast<unsigned>(layoutId), static_cast<unsigned>(status));
            }
        }
        compactReady = compactReady && !compactMessage.empty();
        if (compactReady && ControlQueued(i, preparedControl)) {
            batch.clear();
            auto& value = singleControl[0];
            value = preparedControl.value;
            const bool exact = reliable || preparedControl.exact;
            const bool converted = !exact ||
                npcwire::FromNpcRecord(record, preparedControl.ref.anchor, preparedControl.ref.baseline, true, value);
            if (converted && npcwire::EncodeBatch(controlSender, header.publication, singleControl, batch, exact) &&
                batch.size() <= UINT16_MAX) {
                NpcsHeader controlHeader = header;
                controlHeader.type = MsgControlPose;
                const size_t controlBytes = batch.size() - sizeof(npcwire::WireBatchHeader) + 4;
                const size_t poseSectionBytes = compactMessage.size() - sizeof(header) - sizeof(record);
                controlPoseMessage.resize(sizeof(header) + 2 + controlBytes + poseSectionBytes);
                std::memcpy(controlPoseMessage.data(), &controlHeader, sizeof(controlHeader));
                controlPoseMessage[sizeof(header)] = static_cast<uint8_t>(controlBytes);
                controlPoseMessage[sizeof(header) + 1] = static_cast<uint8_t>(controlBytes >> 8);
                uint8_t* dst = controlPoseMessage.data() + sizeof(header) + 2;
                std::memcpy(dst, batch.data() + 8, 4);
                std::memcpy(dst + 4, batch.data() + sizeof(npcwire::WireBatchHeader),
                            batch.size() - sizeof(npcwire::WireBatchHeader));
                std::memcpy(dst + controlBytes, compactMessage.data() + sizeof(header) + sizeof(record),
                            poseSectionBytes);
            } else if (BoneLog(controlErrors)) {
                Log("npc-converge: compact posed control encode failed; retaining full control with the pose");
            }
        }
        const bool controlReady = compactReady && !controlPoseMessage.empty();
        const bool orderedSetup = compactReady &&
            (!dictionaryReady || (controlReady && !ControlReady(i, preparedControl)));
        const bool sendReliable = reliable || anchor || orderedSetup;
        if (!controlReady) ++npcControlFallback;
        if (!compactReady) {
            if (fallbackBuffer.empty()) {
                fallbackBuffer.resize(fullSize);
                std::memcpy(fallbackBuffer.data() + sizeof(header), &record, sizeof(record));
                std::memcpy(fallbackBuffer.data() + sizeof(header) + sizeof(record), &boneCount, sizeof(boneCount));
                std::memcpy(fallbackBuffer.data() + sizeof(header) + sizeof(record) + sizeof(boneCount), &poseFlags, sizeof(poseFlags));
                fallbackBuffer[sizeof(header) + sizeof(record) + sizeof(boneCount) + 1] = intervalMs;
                std::memcpy(fallbackBuffer.data() + sizeof(header) + sizeof(record) + sizeof(boneCount) + 2,
                            bones, count * sizeof(*bones));
            }
            std::memcpy(fallbackBuffer.data(), &header, sizeof(header));
        }
        const void* data = controlReady ? static_cast<void const*>(controlPoseMessage.data())
            : compactReady ? static_cast<void const*>(compactMessage.data()) : fallbackBuffer.data();
        const size_t bytes = controlReady ? controlPoseMessage.size() : compactReady ? compactMessage.size() : fullSize;
        const size_t poseBytes = compactReady ? compactMessage.size() - sizeof(header) - sizeof(record)
                                              : sizeof(boneCount) + 2 + count * sizeof(pose::Bone);
        if (BoneLog(compactReady ? boneCompactPoseLogs : boneFullPoseLogs))
            Logf("net: player %u pose=%s bones=%d pose_bytes=%u datagram_bytes=%u", static_cast<unsigned>(i),
                 compactReady ? "compact" : "full", count, static_cast<unsigned>(poseBytes),
                 static_cast<unsigned>(bytes));
        if (Send(peers[i], data, bytes, sendReliable ? ChannelDefinitions : ChannelPoses, sendReliable)) {
            ++poseSent;
            queued = true;
            if (compactReady)
                poseCache.Store(header.publication, record.incarnation, layoutId, poseFlags, reconstructed.data(), count,
                                record.sent, poseKey, anchor);
            if (orderedSetup) ++npcOrderedSetupPoses;
            if (anchor) {
                ++npcAnchorsSent;
                if (BoneLog(boneAckLogs))
                    Logf("npc-converge: anchor queued player=%u layout=%u token=%u bones=%d bytes=%zu setup_ordered=%u",
                         static_cast<unsigned>(i), static_cast<unsigned>(layoutId), header.publication, count, bytes,
                         orderedSetup ? 1u : 0u);
            }
            const uint32_t decodedInt = npclease::DecodePoseIntervalWire(intervalMs);
            const uint8_t statsTierInterval = static_cast<uint8_t>((std::min)(decodedInt ? decodedInt : uint32_t(intervalMs), 100u));
            bandwidth.Pose(statsTierInterval, bytes, poseBytes, count, poseKey || !compactReady);
            if (!compactReady) {
                poseStats.headerBytes = 2 + count * sizeof(uint32_t);
                poseStats.rotationBits = count * 144;
                poseStats.translationBits = count * 48;
                poseStats.includedBones = count;
            }
            bandwidth.PoseComponents(poseStats.headerBytes + 2, poseStats.maskBits, poseStats.rotationBits,
                poseStats.translationBits, poseStats.includedBones, poseKey || !compactReady, poseStats.predictive,
                bytes - poseBytes - sizeof(header), sizeof(header), poseStats.rigidBones, poseStats.maxBasisErrorUnits,
                poseStats.PaddingBits());
        } else {
            ++poseSendFailed;
            if (!compactReady && BoneLog(boneFallbackFailureLogs))
                Logf("net: full-hash pose fallback failed for player %u layout=%u",
                     static_cast<unsigned>(i), static_cast<unsigned>(layoutId));
        }
    }
    return queued;
}

void SendPosedNpc(NpcRecord const& record, pose::Bone const* bones, int count, uint8_t poseFlags)
{
    SendPosedNpcTo(0, record, bones, count, poseFlags);
}

} // namespace g3t::net
