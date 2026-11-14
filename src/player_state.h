// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace g3t::net {
#pragma pack(push, 1)
struct PlayerState
{
    uint8_t id = 0;
    uint32_t sequence = 0;
    uint32_t sent = 0;    // the sender's net::Now() when it went out: speed from send times, not arrival (23.10.)
    uint32_t world = 0;   // FNV-1a of the world's file base name
    float position[3] = {};
    float rotation[4] = {};  // quaternion x, y, z, w
    uint8_t flags = 0;       // bit 0: in game (hero present, session running), bit 1: knocked out, bit 2: dead,
                             // bit 3: waiting for help (a death turned into a knock-out, downed.cpp), bit 4: knocked out
                             // by the game's own rules (a guard, not a death): the winner may rob it (ransack.cpp)
                             // While playing: bits 5/6/7 = dialogue/trade/1500-ms interaction close grace.
    uint8_t movementMode = 0;  // gECharMovementMode of the hero: stand, sneak, walk, run, sprint, jump, swim
    uint8_t aniState = 0;      // gEAniState of the hero (stand 2, lying knocked out 16, ...): which motions fit
    char name[24] = {};
    char animation[96] = {};  // the hero's current movement animation (an existing game motion)
    char heldRight[40] = {};  // item names in the hero's hands (drawn weapon, shield, bow); empty = nothing
    char heldLeft[40] = {};
    char shape[32] = {};     // template of the body the player controls instead of its hero (transformed: Wolf), empty = its hero
    char armor[40] = {};     // item names the hero wears in its body and helmet slots (stacks only, not its default body);
                             // empty = none (23.10.: not shown before)
    char helmet[40] = {};
    char look[32] = {};      // template the player chose to be seen as (INI look); empty = the receiver's puppet_template
    int32_t gold = 0;        // the hero's gold while flag bit 4 is set (what a robber finds on its puppet), else 0
    // Protocol 58 (display review 28.10.): the number of the hero's last action motion start (body::ActionSeq; a restart of the
    // same motion counts too) and how far it has played (ms)
    uint8_t actionSeq = 0;
    uint16_t actionMs = 0;
    // Protocol 59 (29.10.): how far slot 0's motion has played, 0..65534 of its length (body::MotionFraction; 65535 only
    // when nothing runs), so the receiver can pace or seek slot 0 to the sender's phase (body.cpp)
    uint16_t motionFrac = 65535;
    uint32_t downedSequence = 0; // the owning player's current downed life; paid revives must echo it
    uint32_t focusNpc = 0, targetNpc = 0; // GUID HashGuid references, 0 = no NPC
};
#pragma pack(pop)

struct PlayerActivity
{
    uint32_t sequence = 0, ticks = 0, world = 0, epoch = 0, downedSequence = 0;
    uint8_t flags = 0; // same full player flags byte; when not playing, bit5=loading and bit6=no running hero/menu
    bool known = false;

    bool Accept(uint32_t next, uint32_t arrived, uint32_t location, uint32_t generation,
                uint8_t state, uint32_t downed)
    {
        if (known && static_cast<int32_t>(next - sequence) <= 0) return false;
        sequence = next;
        ticks = arrived;
        world = location;
        epoch = generation;
        flags = state;
        downedSequence = downed;
        known = true;
        return true;
    }
};
}
