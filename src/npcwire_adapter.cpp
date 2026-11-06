// SPDX-License-Identifier: GPL-3.0-or-later
#include "npcwire_adapter.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace g3t::npcwire {

bool FromNpcRecord(net::NpcRecord const& record, std::uint32_t anchor, std::uint16_t baseline,
                   bool fullRotation, Control& output)
{
    Control value;
    std::copy(record.guid, record.guid + 16, value.guid.begin());
    value.anchor = anchor;
    value.baseline = baseline;
    for (unsigned i = 0; i < 3; ++i) {
        if (!std::isfinite(record.position[i])) return false;
        value.position[i] = record.position[i];
    }
    Quaternion rotation;
    float norm = 0.0f;
    for (unsigned i = 0; i < 4; ++i) {
        if (!std::isfinite(record.rotation[i])) return false;
        rotation.components[i] = record.rotation[i];
        norm += record.rotation[i] * record.rotation[i];
    }
    if (!fullRotation && record.rotation[0] == 0.0f && record.rotation[2] == 0.0f &&
        std::fabs(norm - 1.0f) <= 0.000001f) {
        value.rotation = Yaw{2.0 * std::atan2(static_cast<double>(record.rotation[1]),
                                             static_cast<double>(record.rotation[3]))};
    } else {
        value.rotation = rotation;
    }
    value.sent = record.sent;
    value.movementMode = record.movementMode;
    value.flags = record.flags;
    value.hitPoints = record.hitPoints;
    value.hitPointsMax = record.hitPointsMax;
    value.motion = record.motion;
    value.aniState = record.aniState;
    value.heldRight = record.heldRight;
    value.heldLeft = record.heldLeft;
    value.partyPlayer = record.partyPlayer;
    value.partyType = record.partyType;
    value.waiting = record.waiting;
    value.guidePlayer = record.guidePlayer;
    value.aiMode = record.aiMode;
    value.action = record.action;
    value.attackReason = record.attackReason;
    value.damageType = record.damageType;
    value.damageAmount = record.damageAmount;
    value.damageMultiplier = record.damageMultiplier;
    value.incarnation = record.incarnation;
    value.hitPlayer = record.hitPlayer;
    value.hitSequence = record.hitSequence;
    value.contact = record.contact;
    value.attitude = record.attitude;
    value.combatTarget = record.combatTarget;
    for (unsigned i = 0; i < MotionSlots; ++i) {
        auto const& source = record.motions[i];
        auto& motion = value.motions[i];
        motion.descriptor.name = source.name;
        motion.descriptor.instance = source.instance;
        motion.descriptor.speed = source.speed;
        motion.descriptor.intro = source.intro;
        motion.descriptor.extro = source.extro;
        motion.descriptor.blend = source.blend;
        std::copy(source.options, source.options + 7, motion.descriptor.options.begin());
        motion.phase = source.phase;
        motion.weight = source.weight;
        motion.active = source.active;
    }
    value.slots = ActiveSlots(value);
    output = value;
    return true;
}

bool ToNpcRecord(Control const& value, net::NpcRecord& output)
{
    net::NpcRecord record;
    std::copy(value.guid.begin(), value.guid.end(), record.guid);
    for (unsigned i = 0; i < 3; ++i) {
        if (!std::isfinite(value.position[i]) ||
            std::fabs(value.position[i]) > std::numeric_limits<float>::max()) return false;
        record.position[i] = static_cast<float>(value.position[i]);
    }
    if (auto const* rotation = std::get_if<Quaternion>(&value.rotation)) {
        for (unsigned i = 0; i < 4; ++i) {
            if (!std::isfinite(rotation->components[i])) return false;
            record.rotation[i] = rotation->components[i];
        }
    } else {
        const double yaw = std::get<Yaw>(value.rotation).radians;
        if (!std::isfinite(yaw)) return false;
        record.rotation[0] = record.rotation[2] = 0.0f;
        record.rotation[1] = static_cast<float>(std::sin(yaw * 0.5));
        record.rotation[3] = static_cast<float>(std::cos(yaw * 0.5));
    }
    record.sent = value.sent;
    record.movementMode = value.movementMode;
    record.flags = value.flags;
    record.hitPoints = value.hitPoints;
    record.hitPointsMax = value.hitPointsMax;
    record.motion = value.motion;
    record.aniState = value.aniState;
    record.heldRight = value.heldRight;
    record.heldLeft = value.heldLeft;
    record.partyPlayer = value.partyPlayer;
    record.partyType = value.partyType;
    record.waiting = value.waiting;
    record.guidePlayer = value.guidePlayer;
    record.aiMode = value.aiMode;
    record.action = value.action;
    record.attackReason = value.attackReason;
    record.damageType = value.damageType;
    record.damageAmount = value.damageAmount;
    record.damageMultiplier = value.damageMultiplier;
    record.incarnation = value.incarnation;
    record.hitPlayer = value.hitPlayer;
    record.hitSequence = value.hitSequence;
    record.contact = value.contact;
    record.attitude = value.attitude;
    record.combatTarget = value.combatTarget;
    for (unsigned i = 0; i < MotionSlots; ++i) {
        auto const& motion = value.motions[i];
        auto& target = record.motions[i];
        target.name = motion.descriptor.name;
        target.instance = motion.descriptor.instance;
        target.speed = motion.descriptor.speed;
        target.intro = motion.descriptor.intro;
        target.extro = motion.descriptor.extro;
        target.blend = motion.descriptor.blend;
        std::copy(motion.descriptor.options.begin(), motion.descriptor.options.end(), target.options);
        target.phase = motion.phase;
        target.weight = motion.weight;
        target.active = motion.active;
    }
    output = record;
    return true;
}

}
