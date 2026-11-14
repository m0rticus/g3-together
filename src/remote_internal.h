// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "remote.h"
#include "body.h"
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
    std::string heldItems[2];
    uint32_t dropChecked = 0;
    uint32_t wearChecked = 0;
    uint32_t world = 0;
    uint32_t truthSent = 0;
    uint32_t downedSequence = 0;
    body::State body;
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

GEInt StackFor(Entity& body, char const* name);
void TakeOff(Entity& body, gESlot slot);
}
