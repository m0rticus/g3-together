// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "remote.h"
#include "body.h"
#include "guid_key.h"
#include "movement_policy.h"
#include "net.h"
#include "npcplayback.h"
#include "pose.h"

#include <Game.h>
#include <Script.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace g3t::remote::internal {
struct Puppet
{
    Entity entity;
    bool spawned = false;
    std::string heldRight, heldLeft;
    uint32_t holdChecked = 0;
    GuidKey heldItems[2] = {};
    uint32_t dropChecked = 0;
    uint32_t wearChecked = 0;
    uint32_t world = 0;
    uint32_t truthSent = 0;
    uint32_t downedSequence = 0;
    body::State body;
    movement::PuppetMotionHistory motions;
    std::string shape;
    std::unordered_map<uint32_t, std::string> presentationNames;
    bool terminal = false;
    bool terminalInitialized = false;
    bool terminalSound = false;
    uint32_t terminalTryAt = 0;
    uint32_t terminalFails = 0;
    std::vector<pose::Bone> terminalPose;
    uint8_t terminalPoseFlags = 0;
    npcplayback::State terminalReplay;
};

struct QaState
{
    bool badStates = false;
    bool byContacts = true;
    bool noting = true;
    bool puppetParty = false;
    bool puppetHold = true;
    bool puppetNoDrop = true;
    int puppetLead = -1;
    bool walkLine = false;
    bool puppetCombat = true;
    bool puppetWear = true;
    bool remoteLooks = true;
    bool followShape = true;
};

struct WearStats
{
    unsigned changes = 0;
    unsigned equipped = 0;
    unsigned linked = 0;
    unsigned failed = 0;
};

extern Puppet puppets[net::MaxPlayers];
extern QaState qa;
extern WearStats wearStats;
extern bool haveAnchor;
extern bCVector anchor;

inline GuidKey KeyOf(eCEntity* entity)
{
    const eCEntityProxy proxy(entity);
    GuidKey key{};
    std::memcpy(key.data(), &proxy.GetEntityID().GetCore(), 16);
    return key;
}

inline Entity ByKey(GuidKey const& key)
{
    _GUID raw;
    std::memcpy(&raw, key.data(), 16);
    return Entity(bCPropertyID(bCGuid(raw)));
}

GEInt StackFor(Entity& body, char const* name);
void TakeOff(Entity& body, gESlot slot);
void NoDrop(Puppet& puppet, uint32_t now);
void Wear(Puppet& puppet, char const* armor, char const* helmet, uint32_t now);
void LogPlayerStreams(uint32_t now);
}
