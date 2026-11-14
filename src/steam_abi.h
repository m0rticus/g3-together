// SPDX-License-Identifier: GPL-3.0-or-later
// Combined with Valve's steam_api.dll under the additional permission in COPYING.STEAMWORKS (GPLv3 section 7).
#pragma once

#include "log.h"
#include "steam_diagnostics.h"

#include <cstddef>
#include <cstdint>
#include <float.h>
#include <xmmintrin.h>

namespace g3t::steam {
using u64 = unsigned long long;
using sbool = unsigned char;  // C++ bool, returned in AL
using HSteamPipe = int;

#if defined(_MSC_VER) && defined(_M_IX86)
inline unsigned short FpuControl()
{
    unsigned short value;
    __asm fnstcw value
    return value;
}
#endif

// Real Steam startup changes x87 precision from the engine's single precision to double precision.
class SteamFpScope
{
#if defined(_MSC_VER) && defined(_M_IX86)
    unsigned short control = FpuControl();
#endif
    unsigned mxcsr = _mm_getcsr();
    char const* operation;

  public:
    explicit SteamFpScope(char const* name) : operation(name)
    {
#if defined(_MSC_VER) && defined(_M_IX86)
        const unsigned short libraryControl = static_cast<unsigned short>((control & ~_MCW_PC) | _PC_53);
        __asm fldcw libraryControl
#else
        (void)operation;
#endif
    }
    ~SteamFpScope()
    {
        const unsigned sseAfter = _mm_getcsr();
        static thread_local unsigned changes = 0;
#if defined(_MSC_VER) && defined(_M_IX86)
        const unsigned short after = FpuControl();
        if ((after != control || ((sseAfter ^ mxcsr) & ~0x3fu)) && ++changes <= 8)
            Logf("steam: restoring engine FP state after %s: x87=%04x->%04x mxcsr=%08x->%08x",
                 operation, after, control, sseAfter, mxcsr);
        const unsigned short saved = control;
        __asm fldcw saved
#else
        if (((sseAfter ^ mxcsr) & ~0x3fu) && ++changes <= 8)
            Logf("steam: restoring engine FP state after %s: mxcsr=%08x->%08x",
                 operation, sseAfter, mxcsr);
#endif
        _mm_setcsr(mxcsr);
    }
    SteamFpScope(SteamFpScope const&) = delete;
    SteamFpScope& operator=(SteamFpScope const&) = delete;
};

// The flat API: plain __cdecl functions (the mod's own default is __stdcall, /Gz), the interface first.
struct Api
{
    int(__cdecl* InitFlat)(char* error) = nullptr;
    void(__cdecl* Shutdown)() = nullptr;
    void*(__cdecl* ClientUtils)() = nullptr;
    uint32_t(__cdecl* AppId)(void*) = nullptr;
    HSteamPipe(__cdecl* GetHSteamPipe)() = nullptr;
    void(__cdecl* DispatchInit)() = nullptr;
    void(__cdecl* DispatchRunFrame)(HSteamPipe) = nullptr;
    sbool(__cdecl* DispatchNext)(HSteamPipe, void* message) = nullptr;
    void(__cdecl* DispatchFree)(HSteamPipe) = nullptr;
    sbool(__cdecl* DispatchResult)(HSteamPipe, u64 call, void* buffer, int size, int callback, sbool* failed) = nullptr;
    void*(__cdecl* User)() = nullptr;
    u64(__cdecl* UserSteamId)(void*) = nullptr;
    void*(__cdecl* Matchmaking)() = nullptr;
    u64(__cdecl* CreateLobby)(void*, int type, int maxMembers) = nullptr;
    u64(__cdecl* JoinLobby)(void*, u64 lobby) = nullptr;
    void(__cdecl* LeaveLobby)(void*, u64 lobby) = nullptr;
    sbool(__cdecl* SetLobbyData)(void*, u64 lobby, char const* key, char const* value) = nullptr;
    char const*(__cdecl* GetLobbyData)(void*, u64 lobby, char const* key) = nullptr;
    sbool(__cdecl* RequestLobbyData)(void*, u64 lobby) = nullptr;
    u64(__cdecl* GetLobbyOwner)(void*, u64 lobby) = nullptr;
    int(__cdecl* MemberCount)(void*, u64 lobby) = nullptr;
    u64(__cdecl* MemberByIndex)(void*, u64 lobby, int index) = nullptr;
    void*(__cdecl* Friends)() = nullptr;
    char const*(__cdecl* PersonaName)(void*) = nullptr;
    int(__cdecl* FriendCount)(void*, int flags) = nullptr;
    u64(__cdecl* FriendByIndex)(void*, int index, int flags) = nullptr;
    sbool(__cdecl* FriendGamePlayed)(void*, u64 user, void* info) = nullptr;
    char const*(__cdecl* FriendPersonaName)(void*, u64 user) = nullptr;
    sbool(__cdecl* SetRichPresence)(void*, char const* key, char const* value) = nullptr;
    int(__cdecl* FriendRelationship)(void*, u64 user) = nullptr;  // optional: older stand-ins lack it; 3 = friend
    void*(__cdecl* Messages)() = nullptr;
    int(__cdecl* SendToUser)(void*, void const* identity, void const* data, unsigned size, int flags, int channel) = nullptr;
    int(__cdecl* ReceiveOnChannel)(void*, int channel, void** messages, int max) = nullptr;
    sbool(__cdecl* AcceptSession)(void*, void const* identity) = nullptr;
    sbool(__cdecl* CloseSession)(void*, void const* identity) = nullptr;
    void(__cdecl* IdentitySetSteamId)(void* identity, u64 id) = nullptr;
    u64(__cdecl* IdentityGetSteamId)(void const* identity) = nullptr;
    void(__cdecl* MessageRelease)(void* message) = nullptr;
    void*(__cdecl* Utils)() = nullptr;
    void(__cdecl* InitRelayNetworkAccess)(void*) = nullptr;
    sbool(__cdecl* SetConfigValue)(void*, int value, int scope, intptr_t object, int type, void const* data) = nullptr;
    int(__cdecl* GetConfigValue)(void*, int value, int scope, intptr_t object, int* type, void* data, size_t* size) = nullptr;
    int(__cdecl* GetSessionConnectionInfo)(void*, void const* identity, void* info, void* status) = nullptr;
    void*(__cdecl* AllocateMessage)(void*, int cbAllocateBuffer) = nullptr;
    void*(__cdecl* Sockets)() = nullptr;
    uint32_t(__cdecl* CreateListenSocketP2P)(void*, int localVirtualPort, int nOptions, void const* options) = nullptr;
    uint32_t(__cdecl* ConnectP2P)(void*, void const* identityRemote, int remoteVirtualPort, int nOptions, void const* options) = nullptr;
    int(__cdecl* AcceptConnection)(void*, uint32_t conn) = nullptr;
    sbool(__cdecl* CloseConnection)(void*, uint32_t peer, int reason, char const* debug, sbool linger) = nullptr;
    sbool(__cdecl* CloseListenSocket)(void*, uint32_t socket) = nullptr;
    int(__cdecl* ConfigureConnectionLanes)(void*, uint32_t conn, int numLanes, int const* priorities, uint16_t const* weights) = nullptr;
    void(__cdecl* SendMessages)(void*, int nMessages, void* const* messages, int64_t* outMessageNumberOrResult) = nullptr;
    int(__cdecl* FlushMessagesOnConnection)(void*, uint32_t conn) = nullptr;
    int(__cdecl* ReceiveOnConnection)(void*, uint32_t conn, void** messages, int maxMessages) = nullptr;
    sbool(__cdecl* GetConnectionInfo)(void*, uint32_t conn, void* info) = nullptr;
    int(__cdecl* GetConnectionRealTimeStatus)(void*, uint32_t conn, void* status, int numLanes, void* lanes) = nullptr;
    int(__cdecl* FakeSteamAbi)(void*, uint32_t) = nullptr;
};

struct Export { void** slot; char const* name; };

// Callback ids and transport constants (typed ints avoid MSVC C5287 distinct-enum warnings).
inline constexpr int CbLobbyJoinRequested = 333;   // GameLobbyJoinRequested_t { u64 lobby; u64 friend; }
inline constexpr int CbRichJoinRequested = 337;    // GameRichPresenceJoinRequested_t { u64 friend; char connect[256]; }
inline constexpr int CbLobbyInvite = 503;          // LobbyInvite_t { u64 inviter; u64 lobby; u64 game; }
inline constexpr int CbLobbyEnter = 504;           // LobbyEnter_t { u64 lobby; u32 perms; bool locked; u32 response at 16; }
inline constexpr int CbLobbyDataUpdate = 505;      // LobbyDataUpdate_t { u64 lobby; u64 member; bool success; }
inline constexpr int CbLobbyChatUpdate = 506;      // LobbyChatUpdate_t { u64 lobby; u64 changed; u64 by; u32 state at 24; }
inline constexpr int CbLobbyCreated = 513;         // LobbyCreated_t { int result; u64 lobby at 8; }
inline constexpr int CbCallCompleted = 703;        // SteamAPICallCompleted_t { u64 call; int callback; u32 size; }
inline constexpr int CbConnectionStatusChanged = 1221;
inline constexpr int CbSessionRequest = 1251;
inline constexpr int CbSessionFailed = 1252;

inline constexpr int SendUnreliable = 0;
inline constexpr int SendNoNagle = 1;
inline constexpr int SendReliable = 8;
inline constexpr int SendAutoRestart = 32;
inline constexpr int ResultOk = 1;
inline constexpr int ChannelData = 0;
inline constexpr int ChannelControl = 1;
inline constexpr int P2PVirtualPort = 1;
inline constexpr int LaneCount = 4;
inline constexpr int LobbyFriendsOnly = 1;
inline constexpr int FriendFlagImmediate = 4;
inline constexpr uint32_t GothicAppId = 39500;
inline constexpr char LobbyTag[] = "g3together";
inline constexpr char LobbyVersion[] = "3";

#pragma pack(push, 8)
struct OutboundSocketMessage
{
    void* data = nullptr;
    int32_t size = 0;
    uint32_t conn = 0;
    uint8_t identityPeer[136] = {};
    int64_t connUserData = 0;
    int64_t timeReceived = 0;
    int64_t messageNumber = 0;
    void(__cdecl* freeData)(void*) = nullptr;
    void(__cdecl* release)(void*) = nullptr;
    int32_t channel = 0;
    int32_t flags = 0;
    int64_t userData = 0;
    uint16_t lane = 0;
    uint16_t pad = 0;
};
static_assert(sizeof(void*) != 4 ||
              (offsetof(OutboundSocketMessage, data) == 0 &&
               offsetof(OutboundSocketMessage, size) == 4 &&
               offsetof(OutboundSocketMessage, identityPeer) == 12 &&
               offsetof(OutboundSocketMessage, release) == 0xb4 &&
               offsetof(OutboundSocketMessage, flags) == 0xbc &&
               offsetof(OutboundSocketMessage, lane) == 0xc8),
              "SteamNetworkingMessage_t 32-bit ABI mismatch");

struct DispatchCallbackMsg
{
    int32_t user = 0;
    int32_t callback = 0;
    char* param = nullptr;
    int32_t size = 0;
};
static_assert(sizeof(void*) != 4 ||
              (offsetof(DispatchCallbackMsg, callback) == 4 &&
               offsetof(DispatchCallbackMsg, param) == 8 &&
               offsetof(DispatchCallbackMsg, size) == 12),
              "CallbackMsg_t 32-bit ABI mismatch");

struct CallCompletedParam
{
    u64 call = 0;
    int32_t callback = 0;
    uint32_t size = 0;
};
static_assert(offsetof(CallCompletedParam, callback) == 8 &&
              offsetof(CallCompletedParam, size) == 12,
              "SteamAPICallCompleted_t ABI mismatch");

struct LobbyCreatedParam
{
    int32_t result = 0;
    u64 lobby = 0;
};
static_assert(offsetof(LobbyCreatedParam, lobby) == 8, "LobbyCreated_t ABI mismatch");

struct LobbyEnterParam
{
    u64 lobby = 0;
    uint32_t permissions = 0;
    uint8_t locked = 0;
    uint32_t response = 0;
};
static_assert(offsetof(LobbyEnterParam, response) == 16, "LobbyEnter_t ABI mismatch");

struct LobbyDataUpdateParam
{
    u64 lobby = 0;
    u64 member = 0;
    uint8_t success = 0;
};
static_assert(offsetof(LobbyDataUpdateParam, member) == 8 &&
              offsetof(LobbyDataUpdateParam, success) == 16,
              "LobbyDataUpdate_t ABI mismatch");

struct LobbyChatUpdateParam
{
    u64 lobby = 0;
    u64 userChanged = 0;
    u64 makingChange = 0;
    uint32_t stateChange = 0;
};
static_assert(offsetof(LobbyChatUpdateParam, userChanged) == 8 &&
              offsetof(LobbyChatUpdateParam, stateChange) == 24,
              "LobbyChatUpdate_t ABI mismatch");

struct LobbyJoinRequestedParam
{
    u64 lobby = 0;
    u64 friendId = 0;
};
static_assert(offsetof(LobbyJoinRequestedParam, friendId) == 8, "GameLobbyJoinRequested_t ABI mismatch");

struct LobbyInviteParam
{
    u64 inviter = 0;
    u64 lobby = 0;
    u64 gameId = 0;
};
static_assert(offsetof(LobbyInviteParam, lobby) == 8, "LobbyInvite_t ABI mismatch");

struct ConnectionStatusChangedParam
{
    uint32_t conn = 0;
    diagnostics::ConnectionInfo info{};
    int32_t oldState = 0;
};
static_assert(offsetof(ConnectionStatusChangedParam, info) == 8 &&
              offsetof(ConnectionStatusChangedParam, oldState) == 8 + sizeof(diagnostics::ConnectionInfo),
              "SteamNetConnectionStatusChangedCallback_t ABI mismatch");

struct FriendGameInfoParam
{
    u64 game = 0;
    uint32_t ip = 0;
    uint16_t port = 0;
    uint16_t queryPort = 0;
    u64 lobby = 0;
};
static_assert(offsetof(FriendGameInfoParam, lobby) == 16, "FriendGameInfo_t ABI mismatch");
#pragma pack(pop)
}
