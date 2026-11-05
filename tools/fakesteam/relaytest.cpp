// SPDX-License-Identifier: GPL-3.0-or-later
// The Steam transport without the game: net.cpp, steam.cpp and ENet in a small program, run twice (host and client) with
// the stand-in fake_steam_api.dll. Checks what the game relies on: the client finds the host among its "friends", ENet
// connects through the relay, the hello is welcomed, player states pass both ways, a 2 MB save transfer (file chunks,
// reliable, ENet fragments) arrives whole, a hit claim carries the state the client saw (protocol 57, Stufe 3.3), and a client
// that quits is gone on the host at once.
//
//   set G3T_STEAM_DLL=<build>\fake_steam_api.dll
//   set G3T_FAKESTEAM_DIR=%TEMP%\g3t-relaytest
//   relaytest host 60          (G3T_FAKESTEAM_ID=1)
//   relaytest client 60        (G3T_FAKESTEAM_ID=2), optional 3rd argument: steam_host (a friend's Steam name)
// Each prints RESULT: PASS or RESULT: FAIL with what was missing. Negative control: G3T_FAKESTEAM_INIT=2 on the client
// (Steam not running) must fail startup without opening an alternative connection.
#include <winsock2.h>
#include <windows.h>
#include <enet/enet.h>

#include "config.h"
#include "log.h"
#include "net.h"
#include "parity.h"
#include "steam.h"
#include "lobby.h"
#include "i18n.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>
#include <float.h>
#include <xmmintrin.h>

namespace g3t {
namespace {
Config config;
char const* role = "?";
}
Config const& GetConfig() { return config; }
Config const& LoadConfig() { return config; }
namespace i18n {
char const* GameTextLanguage() { return "English"; }
}

int FormatV(char* out, size_t size, const char* format, va_list args)
{
    if (!size) return 0;
    const int n = std::vsnprintf(out, size, format, args);
    if (n < 0) { *out = '\0'; return 0; }
    return n < static_cast<int>(size) ? n : static_cast<int>(size) - 1;
}

int Format(char* out, size_t size, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const int n = FormatV(out, size, format, args);
    va_end(args);
    return n;
}

void Log(const char* text) { std::printf("[%s %6lu] %s\n", role, GetTickCount() % 1000000, text); std::fflush(stdout); }

void Logf(const char* format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    FormatV(line, sizeof(line), format, args);
    va_end(args);
    Log(line);
}

namespace parity {
uint64_t const* Local()
{
    static uint64_t values[ItemCount] = {1, 2, 3, 4, 5, 6, 7};
    return values;
}
char const* ItemName(int) { return "test"; }
char const* OptionsText() { return "test"; }
}
}

using namespace g3t;

namespace {
constexpr uint32_t SaveSize = 2u << 20;
constexpr uint16_t Chunk = 16000;
std::vector<uint8_t> received(SaveSize);
uint32_t receivedBytes = 0;
bool fileDone = false;
uint32_t doneCrc = 0;

uint32_t Crc(uint8_t const* data, size_t size)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

void OnChunk(uint8_t, uint32_t total, uint32_t offset, uint8_t const* data, uint16_t length)
{
    if (total != SaveSize || offset + length > SaveSize) return;
    std::memcpy(received.data() + offset, data, length);
    receivedBytes += length;
}

void OnDone(char const*, uint32_t const size[net::FileCount], uint32_t const crc[net::FileCount])
{
    fileDone = size[0] == SaveSize;
    doneCrc = crc[0];
}

void PrintStatus(char const* line) { Log(line); }

// Stufe 3.3: the client's hit claim and the send time of the state it saw arrive as sent.
constexpr uint32_t SeenValue = 0x5ee0beefu;
bool hitArrived = false;
bool terminalNpcArrived = false, releaseArrived = false;
bool authorityMode = false, observer = false, acceptedNpcArrived = false, rejectedNpcArrived = false, acceptedReleaseArrived = false;
bool acceptedDeathArrived = false, rejectedDeathArrived = false;
bool acceptedAbsenceArrived = false;
bool acceptedNativeDefaults = false;
bool redirectHit = false, redirectedHitArrived = false;
bool posedNpcArrived = false, canonicalBonesArrived = false;
bool poseFlood = false;
bool poseValidation = false;
unsigned poseValidationValid = 0;
unsigned poseValidationInvalid = 0;
unsigned poseCounts[73] = {};
unsigned offeredPoseFrames = 0;
bool routeClockArrived = false, routeItemArrived = false, routeEnclaveArrived = false;
bool routeWitnessArrived = false, routeDialogArrived = false, routeSummonArrived = false, routeShotArrived = false;
bool routeTargetedItemArrived = false, routeUnknownItemRejected = false;
bool routeNpcSpellProbeArrived = false, routePlayerSpellProbeArrived = false, routeRansackProbeArrived = false;
bool routeWorldExcludedArrived = false, routeWorldBroadcastArrived = false;
bool routeWorldTargetedArrived = false, routeWorldTargetedLeaked = false;
void OnPose(net::NpcRecord const& record, pose::Bone const* bones, int count, uint8_t flags, bool reliable)
{
    if (poseValidation) {
        if (record.guid[0] != 0xc1 || flags || reliable) {
            ++poseValidationInvalid;
            return;
        }
        bool valid = false;
        if (record.guid[1] == 0x10 && count == 1)
            valid = bones[0].name == 17;
        else if (record.guid[1] == 0x11 && count == 95)
            valid = bones[0].name == 95 && bones[94].name == 1;
        else if (record.guid[1] == 0x12 && count == pose::MaxBones)
            valid = bones[0].name == pose::MaxBones && bones[255].name == 1;
        if (!valid) {
            ++poseValidationInvalid;
            return;
        }
        if (record.guid[1] != 0x10) {
            for (int i = 0; i < count; ++i)
                if (bones[i].name != static_cast<uint32_t>(count - i)) {
                    ++poseValidationInvalid;
                    return;
                }
        }
        ++poseValidationValid;
        posedNpcArrived = true;
        return;
    }
    if (record.guid[0] != 0xc1 || count != (poseFlood ? 95 : pose::MaxBones) || flags || reliable) return;
    for (int i = 0; i < count; ++i) {
        if (bones[i].name != static_cast<uint32_t>(i + 1)) return;
        for (int k = 0; k < 12; ++k)
            if (bones[i].values[k] != i * 12 + k) return;
    }
    if (!canonicalBonesArrived) Logf("client: %d canonical bones before NPC state, fragmented without reliability escalation", count);
    canonicalBonesArrived = true;
    if (record.guid[1] < 73) ++poseCounts[record.guid[1]];
    offeredPoseFrames = static_cast<unsigned>(record.position[0]) > offeredPoseFrames
        ? static_cast<unsigned>(record.position[0]) : offeredPoseFrames;
}
int OnNpcs(net::NpcRecord* records, int count, uint32_t, uint32_t, uint8_t owner, uint32_t)
{
    int accepted = 0;
    for (int i = 0; i < count; ++i) {
        if (net::IsHost() && records[i].guid[0] == 0xb2) continue;
        if (owner == 1 && records[i].guid[0] == 0xa1 && records[i].flags == 1 && records[i].hitPoints == 0)
            terminalNpcArrived = true;
        if (owner == 1 && records[i].guid[0] == 0xb1) {
            acceptedNativeDefaults = records[i].motions[0].active == 3;
            if (records[i].absent) acceptedAbsenceArrived = acceptedNpcArrived;
            else acceptedNpcArrived = true;
        }
        if (owner != 1 || records[i].guid[0] == 0xb2) rejectedNpcArrived = true;
        if (owner == 1 && records[i].guid[0] == 0xc1) posedNpcArrived = canonicalBonesArrived;
        records[accepted++] = records[i];
    }
    return accepted;
}
int OnNpcRelease(uint8_t owner, uint32_t, uint8_t* guids, int count)
{
    for (int i = 0; i < count; ++i) {
        if (owner == 1 && guids[i * 16] == 0xa1) releaseArrived = terminalNpcArrived;
        if (owner == 1 && guids[i * 16] == 0xb1) acceptedReleaseArrived = acceptedNpcArrived;
    }
    return count;
}
void OnDeath(uint8_t by, uint8_t const guid[16])
{
    if (!net::IsHost() && by == 1 && guid[0] == 0xb1) acceptedDeathArrived = true;
    else rejectedDeathArrived = true;
}
void OnHitNpc(uint8_t from, uint8_t const guid[16], int32_t delta, uint8_t const weapon[16], uint8_t const npc[16], uint8_t outcome, uint32_t seen,
              uint8_t reaction, uint32_t sequence, uint32_t incarnation)
{
    hitArrived = from == 2 && guid[0] == 0x42 && delta == -7 && outcome == net::HitKnockOut && seen == SeenValue &&
                 reaction == net::HitStumble;
    Logf("host: hit claim from %u delta=%d outcome=%u seen=%08x (%s)", from, delta, outcome, seen, hitArrived ? "as sent" : "WRONG");
    net::HitResult result;
    std::memcpy(result.guid, guid, 16);
    result.sequence = sequence; result.incarnation = incarnation; result.accepted = 1;
    net::SendHitResult(from, result);
}
void OnHitResult(net::HitResult const& result)
{
    redirectedHitArrived = result.accepted && result.sequence && result.incarnation == 1 && result.guid[0] == 0x42;
    Logf("client: acknowledged hit sequence=%u incarnation=%u accepted=%u", result.sequence, result.incarnation, result.accepted);
}
void OnHitPlayer(int32_t, uint8_t const*, uint8_t, bool) {}
void OnRouteItem(uint8_t from, net::ItemNote const& note)
{
    if (from != 1) return;
    routeItemArrived = routeItemArrived || note.op == net::ItemGone;
    routeTargetedItemArrived = routeTargetedItemArrived || note.amount == 2;
    routeUnknownItemRejected = routeUnknownItemRejected || note.amount == 9;
}
void OnRouteEnclave(uint8_t from, net::EnclaveNote const& note) { routeEnclaveArrived = from == 1 && note.op == net::EnclaveState; }
void OnRouteWitness(uint8_t from, net::WitnessNote const& note) { routeWitnessArrived = from == 1 && note.op == net::WitnessHolds; }
void OnRouteDialog(uint8_t from, net::DialogLineNote const& note) { routeDialogArrived = from == 1 && note.kind == net::DialogLineStart; }
void OnRouteSummon(uint8_t from, net::SummonNote const& note) { routeSummonArrived = from == 1 && note.op == net::SummonSpawned; }
void OnRouteShot(uint8_t from, net::ShotNote const& note) { routeShotArrived = from == 1 && note.netId == 0x55aa; }
void OnRouteClock(net::ClockState const& state) { routeClockArrived = state.timestamp == 0x12345678u; }
void OnRouteNpcSpell(uint8_t from, char const* script, uint8_t const*, int32_t)
{
    routeNpcSpellProbeArrived = routeNpcSpellProbeArrived || (from == 1 && std::strcmp(script, "RouteProbe") == 0);
}
void OnRoutePlayerSpell(uint8_t from, char const* script)
{
    routePlayerSpellProbeArrived = routePlayerSpellProbeArrived || (from == 1 && std::strcmp(script, "RouteProbe") == 0);
}
void OnRouteRansack(uint8_t from, net::RansackNote const& note)
{
    routeRansackProbeArrived = routeRansackProbeArrived || (from == 1 && note.seq == 1);
}
void OnRouteWorld(uint8_t from, net::WorldState const& state)
{
    if (from != 1) return;
    routeWorldExcludedArrived = routeWorldExcludedArrived || state.kind == 11;
    routeWorldBroadcastArrived = routeWorldBroadcastArrived || state.kind == 12;
    routeWorldTargetedArrived = routeWorldTargetedArrived || (observer && state.kind == 18);
    routeWorldTargetedLeaked = routeWorldTargetedLeaked || (!observer && !net::IsHost() && state.kind == 18);
}

void PrintMenuStatus()
{
    wchar_t text[700] = {};
    lobby::Format(lobby::Snapshot(), text, 700);
    char utf8[2100] = {};
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), nullptr, nullptr);
    for (char* c = utf8; *c; ++c) if (*c == '\n') *c = '|';
    Logf("MENUSTATUS %s", utf8);
}

int CheckMenuFormatting()
{
    lobby::State state;
    state.phase = lobby::Phase::Hosting;
    state.host = true;
    state.id = 12345678901234567ull;
    state.clients = 2;
    state.readyClients = 1;
    state.linkedPeers = 3;
    strcpy_s(state.name, "SteamHost");
    wchar_t text[700] = {};
    lobby::Format(state, text, 700);
    bool pass = std::wcsstr(text, L"SteamHost") && std::wcsstr(text, L"12345678901234567") &&
                std::wcsstr(text, L"Clients: 2") && std::wcsstr(text, L"ready: 1") && std::wcsstr(text, L"pending");
    state.phase = lobby::Phase::WaitingCampaign;
    state.ping = 42;
    lobby::Format(state, text, 700);
    pass = pass && std::wcsstr(text, L"Waiting for the host's campaign") && std::wcsstr(text, L"42 ms");
    for (char const* language : {"de", "pl"}) {
        strcpy_s(config.language, language);
        lobby::Format(state, text, 700);
        pass = pass && *text && std::wcsstr(text, L"42 ms");
    }
    strcpy_s(config.language, "en");
    state.phase = lobby::Phase::Refused;
    strcpy_s(state.reason, "install differs");
    lobby::Format(state, text, 700);
    pass = pass && std::wcsstr(text, L"install differs");
    Logf("RESULT: %s (menu formatting and localized status)", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

int DirectProbe(unsigned short port, uint32_t seconds)
{
    if (enet_initialize() != 0) return 1;
    ENetAddress local{};
    enet_address_set_host_ip(&local, "127.0.0.1");
    ENetHost* probe = enet_host_create(&local, 1, 3, 0, 0);
    if (!probe) { enet_deinitialize(); return 1; }
    ENetAddress target = local;
    target.port = port;
    ENetPeer* peer = enet_host_connect(probe, &target, 3, 0);
    bool welcomed = false, attempted = false;
    const uint32_t start = GetTickCount();
    while (peer && GetTickCount() - start < seconds * 1000) {
        ENetEvent event;
        while (enet_host_service(probe, &event, 10) > 0) {
            if (event.type == ENET_EVENT_TYPE_CONNECT) {
                // A compatible protocol-59 hello would be accepted by the old direct-IP host.
#pragma pack(push, 1)
                struct Hello { uint8_t type; uint16_t version; char name[24]; uint64_t parity[parity::ItemCount]; };
#pragma pack(pop)
                Hello hello{1, 64, "DirectProbe", {}};
                std::memcpy(hello.parity, parity::Local(), sizeof(hello.parity));
                ENetPacket* packet = enet_packet_create(&hello, sizeof(hello), ENET_PACKET_FLAG_RELIABLE);
                if (!packet || enet_peer_send(peer, 1, packet) < 0) {
                    if (packet) enet_packet_destroy(packet);
                    enet_host_destroy(probe);
                    enet_deinitialize();
                    return 1;
                }
                attempted = true;
            } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                if (event.packet->dataLength && event.packet->data[0] == 2) welcomed = true;
                enet_packet_destroy(event.packet);
            }
        }
    }
    enet_host_destroy(probe);
    enet_deinitialize();
    const bool pass = attempted && !welcomed;
    Logf("RESULT: %s (direct hello attempted %d, welcomed %d)", pass ? "PASS" : "FAIL", attempted, welcomed);
    return pass ? 0 : 1;
}
}

namespace {
int WaitingBudgetProbe()
{
    if (enet_initialize() < 0) { Log("RESULT: FAIL (ENet initialization)"); return 1; }
    ENetHost* probe = enet_host_create(nullptr, 1, 2, 0, 0);
    if (!probe) { enet_deinitialize(); Log("RESULT: FAIL (ENet queue host)"); return 1; }
    probe->maximumWaitingData = 8192;
    ENetAddress address{};
    enet_address_set_host_ip(&address, "127.0.0.1");
    address.port = 9;
    ENetPeer* peer = enet_host_connect(probe, &address, 2, 0);
    bool pass = peer != nullptr;
    if (peer) {
        enet_peer_on_connect(peer);
        peer->state = ENET_PEER_STATE_CONNECTED;
        ENetProtocol held{};
        held.header.command = ENET_PROTOCOL_COMMAND_SEND_FRAGMENT;
        held.header.channelID = 1;
        held.header.reliableSequenceNumber = 1;
        pass = enet_peer_queue_incoming_command(peer, &held, nullptr, 1024, ENET_PACKET_FLAG_RELIABLE, 2) != nullptr;
        for (unsigned i = 0; pass && i < 64; ++i) {
            ENetProtocol fragment{};
            fragment.header.command = ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT;
            fragment.header.channelID = 0;
            fragment.sendFragment.startSequenceNumber = ENET_HOST_TO_NET_16(i * 2 + 1);
            pass = enet_peer_queue_incoming_command(peer, &fragment, nullptr, 4096,
                                                   ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT, 2) != nullptr &&
                   peer->totalWaitingData == 5120;
            if (!pass) break;
            ENetProtocol complete{};
            complete.header.command = ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE;
            complete.header.channelID = 0;
            complete.sendUnreliable.unreliableSequenceNumber = ENET_HOST_TO_NET_16(i * 2 + 2);
            pass = enet_peer_queue_incoming_command(peer, &complete, "q", 1, 0, 0) != nullptr &&
                   peer->totalWaitingData == 1025;
            ENetPacket* received = enet_peer_receive(peer, nullptr);
            pass = pass && received && received->dataLength == 1 && peer->totalWaitingData == 1024;
            if (received) enet_packet_destroy(received);
        }
        enet_peer_reset_queues(peer);
        pass = pass && peer->totalWaitingData == 0;
    }
    enet_host_destroy(probe);
    enet_deinitialize();
    Logf("RESULT: %s (discarded fragments release their exact waiting budget; pending reliable data survives; reset drains all)",
         pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
}

int main(int argc, char** argv)
{
    if (argc >= 2 && !std::strcmp(argv[1], "queueprobe")) {
        role = "queueprobe";
        return WaitingBudgetProbe();
    }
    if (argc == 2 && !std::strcmp(argv[1], "menucheck")) {
        role = "menucheck";
        return CheckMenuFormatting();
    }
    if (argc >= 4 && !std::strcmp(argv[1], "direct")) {
        role = "direct";
        return DirectProbe(static_cast<unsigned short>(std::atoi(argv[2])), static_cast<uint32_t>(std::atoi(argv[3])));
    }
    if (argc < 3 || (std::strcmp(argv[1], "host") && std::strcmp(argv[1], "client") &&
                    std::strcmp(argv[1], "observer") && std::strcmp(argv[1], "lobbyprobe"))) {
        std::printf("usage: relaytest host|client|observer|lobbyprobe <seconds> [steam_host]; direct <test-host-loopback-port> <seconds>\n");
        return 2;
    }
    const bool lobbyProbe = !std::strcmp(argv[1], "lobbyprobe");
    const unsigned fpBefore = _control87(0, 0) & (_MCW_RC | _MCW_PC);
    const unsigned sseBefore = _mm_getcsr() & ~0x3fu;
    bool fpPreserved = true;
    const bool host = lobbyProbe || !std::strcmp(argv[1], "host");
    observer = !std::strcmp(argv[1], "observer");
    authorityMode = observer || std::getenv("G3T_RELAY_AUTHORITY") != nullptr;
    redirectHit = std::getenv("G3T_RELAY_HIT_REDIRECT") != nullptr;
    poseFlood = std::getenv("G3T_RELAY_POSE_FLOOD") != nullptr;
    poseValidation = std::getenv("G3T_RELAY_POSE_VALIDATION") != nullptr;
    role = host ? "host" : "client";
    const uint32_t seconds = static_cast<uint32_t>(std::atoi(argv[2]));
    config.host = host;
    if (argc > 3) strncpy_s(config.steamHost, argv[3], _TRUNCATE);
    std::vector<uint8_t> save(SaveSize);
    for (uint32_t i = 0; i < SaveSize; ++i) save[i] = static_cast<uint8_t>((i * 2654435761u) >> 13);
    const uint32_t saveCrc = Crc(save.data(), save.size());
    net::SetFileHandlers(OnChunk, OnDone);
    net::SetHitHandlers(OnHitNpc, OnHitPlayer);
    net::SetHitResultHandler(OnHitResult);
    net::SetNpcHandlers(OnNpcs, nullptr);
    net::SetNpcPoseHandler(OnPose);
    net::SetNpcReleaseHandler(OnNpcRelease);
    net::SetDeathHandler(OnDeath);
    net::SetClockHandler(OnRouteClock);
    net::SetItemHandler(OnRouteItem);
    net::SetEnclaveHandler(OnRouteEnclave);
    net::SetWitnessHandler(OnRouteWitness);
    net::SetDialogLineHandler(OnRouteDialog);
    net::SetSummonHandler(OnRouteSummon);
    net::SetShotHandler(OnRouteShot);
    net::SetNpcSpellHandler(OnRouteNpcSpell);
    net::SetPlayerSpellHandler(OnRoutePlayerSpell);
    net::SetRansackHandler(OnRouteRansack);
    net::SetWorldStateHandler(OnRouteWorld);

    if (!net::Start(host)) {
        Logf("RESULT: FAIL (required Steam startup: %s; connected=%d joined=%d)",
             net::StartupError(), net::Connected(), net::Joined());
        return 1;
    }
    fpPreserved = (_control87(0, 0) & (_MCW_RC | _MCW_PC)) == fpBefore && (_mm_getcsr() & ~0x3fu) == sseBefore;
    PrintMenuStatus();
    bool sawPeer = false, sentSave = false, sawState = false, peerLeft = false, joined = false, sentHit = false, sentNpcs = false, sentRoutes = false;
    uint32_t joinedAt = 0, leftAfter = 0, hitSentAt = 0, poseSentAt = 0, poseFrame = 0;
    const uint32_t start = GetTickCount();
    float x = 0;
    uint32_t lastMenu = 0;
    while (GetTickCount() - start < seconds * 1000) {
        net::PlayerState local{};
        local.flags = 1;
        local.rotation[3] = 1.0f;
        local.position[0] = x += 1.0f;
        net::Tick(local, true);
        fpPreserved = fpPreserved && (_control87(0, 0) & (_MCW_RC | _MCW_PC)) == fpBefore &&
                      (_mm_getcsr() & ~0x3fu) == sseBefore;
        if (!lastMenu || GetTickCount() - lastMenu >= 1000) {
            lastMenu = GetTickCount();
            PrintMenuStatus();
        }
        if (host) {
            if (poseValidation && sawState && !poseFrame) {
                net::NpcRecord posed{};
                posed.guid[0] = 0xc1;
                posed.rotation[3] = 1.0f;
                posed.hitPoints = 1;
                posed.sent = net::Now();
                posed.presentationReady = 1;

                pose::Bone bones[pose::MaxBones];
                bones[0].name = 17;
                posed.guid[1] = 0x10;
                net::SendPosedNpc(posed, bones, 1);

                for (int i = 0; i < 95; ++i) bones[i].name = static_cast<uint32_t>(95 - i);
                posed.guid[1] = 0x11;
                net::SendPosedNpc(posed, bones, 95);

                for (int i = 0; i < pose::MaxBones; ++i)
                    bones[i].name = static_cast<uint32_t>(pose::MaxBones - i);
                posed.guid[1] = 0x12;
                net::SendPosedNpc(posed, bones, pose::MaxBones);

                for (int i = 0; i < 95; ++i) bones[i].name = static_cast<uint32_t>(i + 1);
                bones[47].name = 0;
                posed.guid[1] = 0xe1;
                net::SendPosedNpc(posed, bones, 95);

                for (int i = 0; i < 95; ++i) bones[i].name = static_cast<uint32_t>(i + 1);
                bones[94].name = bones[47].name;
                posed.guid[1] = 0xe2;
                net::SendPosedNpc(posed, bones, 95);

                poseFrame = 1;
                Log("host: posed bone validation frames queued (1, 95, 256 valid unsorted; zero and duplicate invalid)");
            }
            if (!poseValidation && sawState && (!poseFlood || hitArrived) &&
                (!poseSentAt || GetTickCount() - poseSentAt >= (poseFlood ? 16u : 250u))) {
                if (!poseFrame && std::getenv("G3T_RELAY_POSE_WRAP")) net::PrimePoseWrapForTest();
                net::NpcRecord posed;
                posed.guid[0] = 0xc1;
                posed.rotation[3] = 1.0f;
                posed.hitPoints = 1;
                posed.sent = net::Now();
                posed.position[0] = static_cast<float>(++poseFrame);
                posed.presentationReady = 1;
                pose::Bone bones[pose::MaxBones];
                for (int i = 0; i < pose::MaxBones; ++i) {
                    bones[i].name = i + 1;
                    for (int k = 0; k < 12; ++k) bones[i].values[k] = static_cast<int16_t>(i * 12 + k);
                }
                for (int i = 0; i < (poseFlood ? 73 : 1); ++i) {
                    posed.guid[1] = static_cast<uint8_t>(i);
                    net::SendPosedNpc(posed, bones, poseFlood ? 95 : pose::MaxBones);
                }
                poseSentAt = GetTickCount();
            }
            if (authorityMode && net::PeerJoined(3) && net::InWorld(3) && !sentNpcs) {
                net::NpcRecord canonical{};
                canonical.guid[0] = 0xb1; canonical.rotation[3] = 1.0f; canonical.hitPoints = 100;
                canonical.motions[0].active = 3;
                canonical.motions[0].name = 7; canonical.motions[0].instance = 23; canonical.motions[0].weight = 1.0f;
                net::SendNpcs(&canonical, 1, true);
                canonical.absent = 1;
                net::SendNpcs(&canonical, 1, true);
                net::SendNpcRelease(canonical.guid, 1);
                net::SendDeath(canonical.guid, 1, 0);
                net::ItemNote target{};
                target.op = net::ItemDenied;
                target.amount = 2;
                net::SendItem(target, 2);
                net::EnclaveNote broadcast{};
                broadcast.op = net::EnclaveState;
                net::SendEnclave(broadcast, 0);
                net::WorldState state{};
                state.kind = 18;
                net::SendWorldState(state, 2);
                state.kind = 12;
                net::SendWorldState(state, 0);
                net::SendDeath(canonical.guid, 1, 2);
                sentNpcs = true;
            }
            for (uint8_t id = 3; id < net::MaxPlayers; ++id)
                if (net::PeerJoined(id) && !net::InWorld(id)) net::SetInWorld(id, true);
            if (!sawPeer && net::PeerJoined(2)) {
                sawPeer = true;
                PrintMenuStatus();
                net::SetInWorld(2, true);
                PrintMenuStatus();
                Log("host: client 2 joined, sending the save");
            }
            if (sawPeer && net::InWorld(2) && !sentRoutes) {
                net::ClockState clock{};
                clock.timestamp = 0x12345678u;
                net::SendClock(clock);
                net::DialogLineNote line{};
                line.kind = net::DialogLineStart;
                net::SendDialogLine(line);
                net::SummonNote summon{};
                summon.op = net::SummonSpawned;
                summon.netId = 0x55aa;
                net::SendSummon(summon);
                net::ShotNote shot{};
                shot.netId = 0x55aa;
                net::SendShot(shot);
                net::ItemNote item{};
                item.op = net::ItemGone;
                item.amount = 1;
                net::SendItem(item, 0);
                item.amount = 2;
                net::SendItem(item, 2);
                item.amount = 9;
                net::SendItem(item, 9);
                net::EnclaveNote enclave{};
                enclave.op = net::EnclaveState;
                net::SendEnclave(enclave, 0);
                net::WitnessNote witness{};
                witness.op = net::WitnessHolds;
                net::SendWitness(0, witness);
                uint8_t target[16] = {};
                net::SendNpcSpell(0, "RouteProbe", target, 7);
                net::SendNpcSpell(net::LocalId(), "RouteGuard", target, 7);
                net::SendNpcSpell(9, "RouteUnknown", target, 7);
                net::SendPlayerSpell(0, "RouteProbe");
                net::SendPlayerSpell(net::LocalId(), "RouteGuard");
                net::SendRansack(0, net::RansackNote{net::RansackHold, 1, 3, {}});
                net::SendRansack(net::LocalId(), net::RansackNote{net::RansackHold, 2, 3, {}});
                net::WorldState state{};
                state.kind = 11;
                net::SendWorldState(state, 2);
                state.kind = 12;
                net::SendWorldState(state, 0);
                sentRoutes = true;
            }
            if (sawPeer && !sentSave) {
                bool ok = true;
                for (uint32_t offset = 0; offset < SaveSize && ok; offset += Chunk) {
                    const uint16_t length = static_cast<uint16_t>(SaveSize - offset < Chunk ? SaveSize - offset : Chunk);
                    ok = net::SendFileChunk(2, 0, SaveSize, offset, save.data() + offset, length);
                }
                uint32_t sizes[net::FileCount] = {SaveSize}, crcs[net::FileCount] = {saveCrc};
                ok = ok && net::SendFileDone(2, "relaytest", sizes, crcs);
                sentSave = true;
                Logf("host: save of %u bytes queued (%s)", SaveSize, ok ? "ok" : "FAILED");
            }
            if (sawPeer && net::Remotes()[2].latest.position[0] > 0 && !sawState) {
                sawState = true;
                Logf("host: client state arrived (x = %.0f)", net::Remotes()[2].latest.position[0]);
                net::NpcRecord terminal{};
                terminal.guid[0] = 0xa1;
                terminal.flags = 1;
                terminal.rotation[3] = 1.0f;
                net::SendNpcs(&terminal, 1, true);
                net::SendNpcRelease(terminal.guid, 1);
            }
            if (sawState && !net::PeerJoined(2) && !peerLeft) {
                peerLeft = true;
                Log("host: client 2 is gone");
                net::ItemNote disconnected{};
                disconnected.op = net::ItemDenied;
                disconnected.amount = 7;
                net::SendItem(disconnected, 2);
                break;
            }
        } else {
            if (!joined && net::Joined()) {
                joined = true;
                joinedAt = GetTickCount();
                PrintMenuStatus();
                net::SetInWorld(net::LocalId(), true);
                PrintMenuStatus();
                Logf("client: welcomed as player %u", net::LocalId());
            }
            if (joined && net::Remotes()[1].latest.position[0] > 0 && !sawState) {
                sawState = true;
                Logf("client: host state arrived (x = %.0f)", net::Remotes()[1].latest.position[0]);
                Logf("client: host Steam wire name=%s", net::PeerName(1));
            }
            if (sawState && !sentHit && !observer && (!poseFlood || fileDone) &&
                (!authorityMode || net::Remotes()[3].connected)) {
                uint8_t guid[16] = {0x42}, weapon[16] = {};
                net::SendHitNpc(1, guid, -7, weapon, nullptr, net::HitKnockOut, SeenValue, net::HitStumble);
                sentHit = true;
                hitSentAt = GetTickCount();
                Log("client: hit claim sent");
            }
            if (authorityMode && !observer && sawState && !sentNpcs && net::Remotes()[3].connected) {
                net::NpcRecord records[2]{};
                records[0].guid[0] = 0xb2;
                records[1].guid[0] = 0xb2;
                for (auto& record : records) { record.rotation[3] = 1.0f; record.hitPoints = 100; }
                net::SendNpcs(records, 2, true);
                net::SendNpcRelease(records[0].guid, 1);
                net::SendDeath(records[0].guid, net::LocalId(), 0);
                sentNpcs = true;
            }
            if (observer && acceptedNpcArrived && acceptedAbsenceArrived && acceptedReleaseArrived &&
                acceptedDeathArrived && posedNpcArrived && routeEnclaveArrived &&
                !routeTargetedItemArrived && !routeUnknownItemRejected &&
                !routeNpcSpellProbeArrived && !routePlayerSpellProbeArrived && !routeRansackProbeArrived &&
                !routeWorldExcludedArrived && routeWorldBroadcastArrived && routeWorldTargetedArrived) {
                if (!hitSentAt) hitSentAt = GetTickCount();
                if (GetTickCount() - hitSentAt > 500) break;
            }
            if (!observer && fileDone && sawState && sentHit && terminalNpcArrived && releaseArrived && posedNpcArrived &&
                (!authorityMode || sentNpcs) && (!redirectHit || redirectedHitArrived) &&
                routeClockArrived && routeItemArrived && routeEnclaveArrived && routeWitnessArrived &&
                routeDialogArrived && routeSummonArrived && routeShotArrived && routeTargetedItemArrived &&
                !routeUnknownItemRejected &&
                !routeNpcSpellProbeArrived && !routePlayerSpellProbeArrived && !routeRansackProbeArrived &&
                !routeWorldExcludedArrived && routeWorldBroadcastArrived && !routeWorldTargetedLeaked &&
                GetTickCount() - hitSentAt > (poseFlood ? 5000u : authorityMode ? 3000u : 500u)) {
                leftAfter = GetTickCount() - joinedAt;
                break;
            }
        }
        Sleep(16);
    }
    steam::Status(PrintStatus);
    PrintMenuStatus();
    unsigned acceptedNpcs = 0, rejectedNpcs = 0, staleNpcs = 0;
    net::NpcReplicationCounts(acceptedNpcs, rejectedNpcs, staleNpcs);
    bool pass = false;
    if (host) {
        if (lobbyProbe) {
            pass = steam::Ready() && steam::LobbyId() && lobby::Snapshot().phase == lobby::Phase::Hosting;
            Logf("RESULT: %s (Steam startup, real lobby and hosting menu state; no peer gameplay tested)", pass ? "PASS" : "FAIL");
        } else {
            if (poseValidation) {
                pass = sawPeer && sentSave && sawState;
                Logf("RESULT: %s (peer %d, save sent %d, client state %d, pose validation sent)",
                     pass ? "PASS" : "FAIL", sawPeer, sentSave, sawState);
            } else {
                pass = sawPeer && sentSave && sawState && peerLeft && hitArrived && !rejectedDeathArrived &&
                       (!authorityMode || sentNpcs);
                Logf("RESULT: %s (peer %d, save sent %d, client state %d, hit claim %d, client left %d, death claims %d)",
                     pass ? "PASS" : "FAIL", sawPeer, sentSave, sawState, hitArrived, peerLeft, rejectedDeathArrived);
            }
        }
    } else if (observer) {
        pass = joined && sawState && acceptedNpcArrived && acceptedReleaseArrived && !rejectedNpcArrived &&
               acceptedAbsenceArrived && acceptedDeathArrived && !rejectedDeathArrived && acceptedNativeDefaults && posedNpcArrived &&
               routeEnclaveArrived && !routeTargetedItemArrived && !routeUnknownItemRejected &&
               !routeNpcSpellProbeArrived && !routePlayerSpellProbeArrived && !routeRansackProbeArrived &&
               !routeWorldExcludedArrived && routeWorldBroadcastArrived && routeWorldTargetedArrived;
        Logf("RESULT: %s (host-approved NPC %d, ordered release %d, refused NPC leaked %d, host death %d, refused death leaked %d, host removal %d, native defaults %d, world route targeted %d)",
             pass ? "PASS" : "FAIL", acceptedNpcArrived, acceptedReleaseArrived, rejectedNpcArrived,
             acceptedDeathArrived, rejectedDeathArrived, acceptedAbsenceArrived, acceptedNativeDefaults, routeWorldTargetedArrived);
    } else {
        const uint32_t got = Crc(received.data(), received.size());
        const bool whole = fileDone && receivedBytes == SaveSize && got == saveCrc && doneCrc == saveCrc;
        if (poseValidation) {
            pass = joined && sawState && whole && poseValidationValid == 3 &&
                   poseValidationInvalid == 0 && rejectedNpcs >= 2 && !routeWorldTargetedLeaked;
            Logf("POSEVALIDATION valid=%u invalid_callback=%u rejected=%u stale=%u world route targeted leaked=%d",
                 poseValidationValid, poseValidationInvalid, rejectedNpcs, staleNpcs, routeWorldTargetedLeaked);
        } else {
            pass = joined && sawState && whole && *steam::HostName() && terminalNpcArrived && releaseArrived && posedNpcArrived &&
                   (!redirectHit || redirectedHitArrived) && (!authorityMode || acceptedDeathArrived) && !rejectedDeathArrived &&
                   routeClockArrived && routeItemArrived && routeEnclaveArrived && routeWitnessArrived &&
                   routeDialogArrived && routeSummonArrived && routeShotArrived && routeTargetedItemArrived &&
                   !routeUnknownItemRejected && !routeNpcSpellProbeArrived && !routePlayerSpellProbeArrived &&
                   !routeRansackProbeArrived && !routeWorldExcludedArrived && routeWorldBroadcastArrived && !routeWorldTargetedLeaked;
            Logf("RESULT: %s (welcomed %d, host state %d, save %u of %u bytes, crc %s, host %s, %u ms after the welcome, world route targeted leaked %d)",
                 pass ? "PASS" : "FAIL", joined, sawState, receivedBytes, SaveSize, got == saveCrc ? "ok" : "WRONG",
                 *steam::HostName() ? steam::HostName() : "-", leftAfter, routeWorldTargetedLeaked);
        }
        if (poseFlood) {
            unsigned minimum = poseCounts[0], maximum = poseCounts[0];
            for (auto count : poseCounts) { minimum = count < minimum ? count : minimum; maximum = count > maximum ? count : maximum; }
            Logf("POSEFLOOD actors=73 minimum=%u maximum=%u offered_frames=%u minimum_delivery=90%%", minimum, maximum, offeredPoseFrames);
                        pass = pass && offeredPoseFrames > 100 && minimum * 100 >= offeredPoseFrames * 90;
        }
        // Wait for the host's last messages, then quit as a player does: the host must notice at once.
    }
    net::Stop();
    fpPreserved = fpPreserved && (_control87(0, 0) & (_MCW_RC | _MCW_PC)) == fpBefore &&
                  (_mm_getcsr() & ~0x3fu) == sseBefore;
    if (std::getenv("G3T_RELAY_CHECK_FP")) {
        Logf("FPSTATE preserved=%d", fpPreserved);
        pass = pass && fpPreserved;
    }
    for (int i = 0; i < 20; ++i) { Sleep(10); }
    return pass ? 0 : 1;
}
