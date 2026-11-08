// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "session.h"
#include "npcremoval_policy.h"
#include <map>
#include <string>

namespace g3t {
struct Config
{
    bool host = true;               // role = host | client
    char sessionId[SessionIdSize] = "default";  // shared, case-sensitive session_id; not the native Steam lobby number
    bool sessionIdValid = true;
    // Steam is the only transport. ENet's sockets are private loopback bridges, not direct-IP endpoints.
    // connection = invite | auto (steam.cpp, steam_connect.h). invite (default): nothing connects, searches or creates a lobby on
    // its own; the host publishes Join Game rich presence and the client connects only after its player accepts a Steam invite /
    // Join Game. auto: the legacy behaviour (host opens a lobby at startup, client scans the friends list), kept for the stand-in/tests.
    bool connectionAuto = false;
    char steamLobby[24] = "";             // client, steam: join this lobby (its number from the host's log) instead of searching; invite mode: manual join at startup
    char steamHost[64] = "";              // client, steam: only this friend's game (its Steam name); invite mode: manual join of that friend at startup
    std::map<std::string, uint64_t> legacyCompanionOwners; // host-authorized one-time migration, old label -> SteamID64
    char language[5] = "en";       // local HUD language: en | de | pl | auto (the game's text language); not a network setting
    char puppetTemplate[64] = "PC_Hero";  // template used to show other players (the hero himself) who chose no look
    char look[32] = "";                  // this player's own look: the template the others show it as (empty: theirs)
    char partnerKey[16] = "PAGEDOWN";     // key that jumps to the other player (partner.cpp)
    char trace[8] = "off";                // off | on | all: runtime trace to g3trace.log (trace.cpp)
    // Town reputation (enclaves.cpp; the host's setting counts): shared = one town counter for both, the guard pursues
    // the one who did it; together = shared, and both answer for it; personal = each player's own counters.
    char reputation[12] = "shared";
    // host_roi: desired NPC coverage in cm. Extends NPC collection only; never shrinks or changes the engine's native ROI.
    float hostRoi = 8000.0f;
    uint8_t ardeaRemoveNpcs = npcremoval::policy::Defaults; // host: reviewed Ardea instances only; empty list disables
    bool npcAuthorityFallback = true; // npc_authority_fallback = on | off; restricted wildlife leases
    // host_sim = elastic | off: elastic adds one NPC-only range covering the players; the real camera never moves.
    bool hostSimElastic = false;
    // host_sim = spheres (world.cpp, Stufe 2 in full, 28.10.): instead of one sphere around all, the host processes one sphere
    // around each other player as well as its own (host_roi each), without changing static-world culling. Default since 30.10. (owner;
    // stageRoiHold at 60 m: spheres 3/3, host_sim = off red).
    bool hostSimSpheres = true;
    // puppet_lead = auto | off (body.cpp, Stufe 3.2): auto shows the other players' puppets where they probably are now (their
    // last movement carried on by half the round trip, at most 150 ms) instead of where their last state came from; off: as
    // before. Each player for the puppets on its own screen. Auto by default since 28.10.
    bool puppetLead = true;
    // motion_seek = on | off (body.cpp, display step 4, 29.10.): on shows the other machine's blows (its puppet, mirrored NPCs)
    // where the sender's motion is now instead of from their start. On by default since 29.10. (stageSeek under NETSIM 80,40,3:
    // p95 off 315-432 ms, on 66-134 ms).
    bool motionSeek = true;
    bool movementSmoothing = true;  // movement_smoothing = on | off; buffered replica poses, not local input delay
    // Player state defaults to 30 Hz; reliable gameplay is not rate limited.
    int sendRate = 30;
    int networkBudgetKib = 256; // per outgoing peer, 64..4096 KiB/s admission budget
    // listener_textbox = off | on (dialogaudio.cpp, owner 28.10.): off (default; stageDialogOptions 28.10.: no box, the lines as
    // subtitles) a player who only listens to the other's dialog sees its lines as subtitles, as the game shows the talk of
    // others, but not the speaker's menu of answers in a box; on: the menu too, as before.
    bool listenerTextbox = false;
    // Session balance follows the host in memory; each player's INI stays untouched.
    // friendly_fire = percent of cross-player party damage. Zero (default) blocks harmful contacts between
    // all co-op party members before damage or reactions; positive values retain damage scaling. npc_damage = factor on
    // the damage NPCs deal to players and companions, npc_hp = factor on how
    // much NPCs endure (the damage they take is divided by it), both default 1.0, companions excepted.
    int friendlyFire = 0;
    float npcDamage = 1.0f;
    float npcHp = 1.0f;
    // client_autoload = on | off (persist.cpp, client only, local choice): when the host loads a save or starts a campaign the
    // client returns to its menu and, after a cancelable countdown, enters the host's world with its own account character.
    bool clientAutoload = true;
};

// Reads <game dir>\g3together.ini (key = value lines, # comments). Missing file: defaults, host role.
Config const& LoadConfig();
Config const& GetConfig();
Config const& GetLocalConfig(); // stored local preferences, without session overlays
}
