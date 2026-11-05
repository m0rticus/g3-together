// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace g3t {
struct Config
{
    bool host = true;               // role = host | client
    // Steam is the only transport. ENet's sockets are private loopback bridges, not direct-IP endpoints.
    char steamLobby[24] = "";             // client, steam: join this lobby (its number from the host's log) instead of searching
    char steamHost[64] = "";              // client, steam: only this friend's game (its Steam name), when several friends host
    char language[5] = "en";       // local HUD language: en | de | pl | auto (the game's text language); not a network setting
    char puppetTemplate[64] = "PC_Hero";  // template used to show other players (the hero himself) who chose no look
    char look[32] = "";                  // this player's own look: the template the others show it as (empty: theirs)
    char partnerKey[16] = "PAGEDOWN";     // key that jumps to the other player (partner.cpp)
    char qaHostHint[16] = "";             // QA negative controls, only with the QA bot: off (no menu hints), norefusal (no refusal there)
    bool qaResendGuardOff = false;  // qa_resendguard = off: the host sends its save again while one is on its way (persist.cpp)
    char trace[8] = "off";                // off | on | all: runtime trace to g3trace.log (trace.cpp)
    // Town reputation (enclaves.cpp; the host's setting counts): shared = one town counter for both, the guard pursues
    // the one who did it; together = shared, and both answer for it; personal = each player's own counters.
    char reputation[12] = "shared";
    // host_roi: desired NPC coverage in cm. Extends NPC collection only; never shrinks or changes the engine's native ROI.
    float hostRoi = 8000.0f;
    // host_sim = elastic | off: elastic adds one NPC-only range covering the players; the real camera never moves.
    bool hostSimElastic = false;
    // host_sim = spheres (world.cpp, Stufe 2 in full, 28.10.): instead of one sphere around all, the host processes one sphere
    // around each other player as well as its own (host_roi each), without changing static-world culling. Default since 30.10. (owner;
    // stageRoiHold at 60 m: spheres 3/3, host_sim = off red).
    bool hostSimSpheres = true;
    // puppet_lead = auto | off (body.cpp, Stufe 3.2): auto shows the other players' puppets where they probably are now (their
    // last movement carried on by half the round trip, at most 150 ms) instead of where their last state came from; off: as
    // before. Each player for the puppets on its own screen. Auto by default since 28.10. (stageWalk under G3_NETSIM: the
    // puppet's still frames in curves about halved).
    bool puppetLead = true;
    // motion_seek = on | off (body.cpp, display step 4, 29.10.): on shows the other machine's blows (its puppet, mirrored NPCs)
    // where the sender's motion is now instead of from their start. On by default since 29.10. (stageSeek under NETSIM 80,40,3:
    // p95 off 315-432 ms, on 66-134 ms).
    bool motionSeek = true;
    bool movementSmoothing = true;  // movement_smoothing = on | off; buffered replica poses, not local input delay
    // send_rate = 20..60 (Hz, default 60): how often player states and every NPC that changed are sent. A higher rate lets the
    // receiving side show them with a shorter display buffer; unchanged NPCs still go out only every 100 ms.
    int sendRate = 60;
    // listener_textbox = off | on (dialogaudio.cpp, owner 28.10.): off (default; stageDialogOptions 28.10.: no box, the lines as
    // subtitles) a player who only listens to the other's dialog sees its lines as subtitles, as the game shows the talk of
    // others, but not the speaker's menu of answers in a box; on: the menu too, as before.
    bool listenerTextbox = false;
    // Owner 28.10., as in single player but adjustable (combat.cpp; each player's INI, compared at the join like the game's
    // options): friendly_fire = percent of cross-player party damage. Zero (default) blocks harmful contacts between
    // all co-op party members before damage or reactions; positive values retain damage scaling. npc_damage = factor on
    // the damage NPCs deal to players and companions, npc_hp = factor on how
    // much NPCs endure (the damage they take is divided by it), both default 1.0, companions excepted.
    int friendlyFire = 0;
    float npcDamage = 1.0f;
    float npcHp = 1.0f;
};

// Reads <game dir>\g3together.ini (key = value lines, # comments). Missing file: defaults, host role.
Config const& LoadConfig();
Config const& GetConfig();
}
