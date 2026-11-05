// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "net.h"
#include <unordered_map>

class eCEntity;

namespace g3t::npcplayback {
using LogFn = void (*)(char const*);
using PublishName = void (*)(uint32_t, char const*);
using FindName = char const* (*)(uint32_t);
struct State {
    uint32_t instances[net::NpcMotionSlots] = {};
    uint32_t suppressed[net::NpcMotionSlots] = {};
    uint32_t effects[net::NpcMotionSlots] = {};
    uint32_t incarnation = 0, pendingHit = 0, acknowledgedHit = 0;
    uint32_t localStart = 0;
    uint32_t acknowledgedAt = 0;
    float localDuration = 0.0f;
    char localMotion[96] = {};
    net::NpcMotion localOptions;
    void const* nodes = nullptr;
    int nodeCount = 0;
    std::unordered_map<uint32_t, int> boneIndices;
    bool initialized = false;
};
class HitOrigin {
public:
    HitOrigin(eCEntity* entity, uint8_t player, uint32_t sequence);
    ~HitOrigin();
    HitOrigin(HitOrigin const&) = delete;
    HitOrigin& operator=(HitOrigin const&) = delete;
private:
    eCEntity* before;
    uint8_t beforePlayer;
    uint32_t beforeSequence;
};
bool Start(bool client);
bool Available();
bool IsPassive(eCEntity const* entity);
void Silence(eCEntity* entity);  // stop and clear task/state restored directly by a native save or processing-range entry
uint32_t MotionInstance(eCEntity* entity, int slot = 0);
void BeginHit(eCEntity const* entity);
char const* HitTask(eCEntity const* entity);
void Stats(LogFn log);
void UpdatePresentation(eCEntity* entity);
bool Capture(eCEntity* entity, net::NpcRecord& record, PublishName publish);
bool CapturePose(eCEntity* entity, std::vector<pose::Bone>& bones, uint8_t& flags, PublishName publish);
bool Replay(eCEntity* entity, State& state, net::NpcRecord const& record, FindName names, uint32_t now);
bool ReplayFrozenPose(eCEntity* entity, State& state, std::vector<pose::Bone> const& bones, uint8_t flags);
bool LocalHit(eCEntity* entity, State& state, char const* motion, net::NpcMotion const& options, uint32_t hit, uint32_t now);
void Tick();
void OnWorldDeactivate();
void Probe(char const* argument, LogFn log);  // QA: source-target NPC names, report, or off
}
