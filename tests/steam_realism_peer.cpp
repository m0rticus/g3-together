// SPDX-License-Identifier: GPL-3.0-or-later
#include <winsock2.h>
#include <windows.h>

namespace {
static FARPROC WINAPI ObservedGetProcAddress(HMODULE, LPCSTR);
static DWORD WINAPI ObservedWorkerWait(DWORD, HANDLE const*, BOOL, DWORD);
}
#define GetProcAddress ObservedGetProcAddress
#define WaitForMultipleObjects ObservedWorkerWait
#include G3T_REALISM_SOURCE
#undef WaitForMultipleObjects
#undef GetProcAddress

#include "enet/enet.h"
#include "enet_policy.h"
#include "relaywire.h"
#include "steam.h"
#include "steam_send.h"
#include "fake_steam_diag.h"

#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

struct NativeFrameCapture
{
    std::mutex mutex;
    std::vector<std::vector<unsigned char>> frames;
};

static NativeFrameCapture nativeFrameCapture;

extern "C" int RealismNativeSocketSend(
    ENetSocket socket,
    ENetAddress const* address,
    ENetBuffer const* buffers,
    size_t bufferCount)
{
    std::vector<unsigned char> captured;
    size_t capturedBytes = 0;
    for (size_t i = 0; i < bufferCount; ++i) {
        capturedBytes += buffers[i].dataLength;
    }
    if (capturedBytes <= g3t::steam::RelayPayloadLimit) {
        captured.reserve(capturedBytes);
        for (size_t i = 0; i < bufferCount; ++i) {
            auto const* bytes = static_cast<unsigned char const*>(buffers[i].data);
            captured.insert(captured.end(), bytes, bytes + buffers[i].dataLength);
        }
    }
    const int result = enet_socket_send(socket, address, buffers, bufferCount);
    if (result > 0 && captured.size() == capturedBytes) {
        std::lock_guard<std::mutex> lock(nativeFrameCapture.mutex);
        nativeFrameCapture.frames.push_back(std::move(captured));
    }
    return result;
}

namespace g3t {
Config probeConfig;
Config const& GetConfig() { return probeConfig; }
Config const& GetLocalConfig() { return probeConfig; }

void Log(char const* text) { if (text) std::puts(text); }
void Logf(char const* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::putchar('\n');
}
int FormatV(char* out, size_t size, char const* format, va_list args)
{
    if (!out || !size) return 0;
    const int result = _vsnprintf_s(out, size, _TRUNCATE, format, args);
    if (result < 0) {
        out[size - 1] = '\0';
        return static_cast<int>(std::strlen(out));
    }
    return result;
}
int Format(char* out, size_t size, char const* format, ...)
{
    va_list args;
    va_start(args, format);
    const int result = FormatV(out, size, format, args);
    va_end(args);
    return result;
}
namespace crashlog { void PollWorldEntry() {} }
}

namespace {
struct Args
{
    bool host = true;
    uint64_t hostId = 76561198011111111ULL;
    uint64_t clientId = 76561198022222222ULL;
    char session[64] = "realism";
    char role[16] = "host";
    char profile[32] = "normal";
    char out[1024] = "";
    char sourceRoot[1024] = "";
    char scenario[32] = "connect";
    uint32_t seconds = 5;
    uint32_t appBps = 400000;
    uint32_t physicalBps = 262144;
    uint32_t metadataDelayMs = 0;
    uint16_t enetPort = 0;
    uint32_t leaveMs = 0;
    bool seeded = false;
};

Args ParseArgs(int argc, char** argv)
{
    Args args;
    auto take = [&](int& i, char const* name, char* out, size_t size) {
        if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", name); std::exit(2); }
        std::strncpy(out, argv[++i], size - 1);
        out[size - 1] = '\0';
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--role" && i + 1 < argc) take(i, "--role", args.role, sizeof(args.role));
        else if (a == "--scenario" && i + 1 < argc) take(i, "--scenario", args.scenario, sizeof(args.scenario));
        else if (a == "--profile" && i + 1 < argc) take(i, "--profile", args.profile, sizeof(args.profile));
        else if (a == "--session" && i + 1 < argc) take(i, "--session", args.session, sizeof(args.session));
        else if (a == "--out" && i + 1 < argc) take(i, "--out", args.out, sizeof(args.out));
        else if (a == "--source-root" && i + 1 < argc) take(i, "--source-root", args.sourceRoot, sizeof(args.sourceRoot));
        else if (a == "--host-id" && i + 1 < argc) args.hostId = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--client-id" && i + 1 < argc) args.clientId = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--seconds" && i + 1 < argc) args.seconds = std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--app-bps" && i + 1 < argc) args.appBps = std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--physical-bps" && i + 1 < argc) args.physicalBps = std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--enet-port" && i + 1 < argc) args.enetPort = static_cast<uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--leave-at-ms" && i + 1 < argc) args.leaveMs = std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--seeded") args.seeded = true;
        else if (a.rfind("--", 0) == 0) { std::fprintf(stderr, "unknown argument: %s\n", argv[i]); std::exit(2); }
    }
    args.host = std::strcmp(args.role, "host") == 0;
    args.metadataDelayMs = std::strtoul(getenv("G3T_FAKESTEAM_METADATA_DELAY_MS") ? getenv("G3T_FAKESTEAM_METADATA_DELAY_MS") : "0", nullptr, 10);
    if (!args.host && std::strcmp(args.role, "client") != 0) {
        std::fprintf(stderr, "role must be host or client\n");
        std::exit(2);
    }
    return args;
}

uint64_t QpcTicks()
{
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return static_cast<uint64_t>(v.QuadPart);
}

double QpcFrequency()
{
    static double frequency = [] {
        LARGE_INTEGER v{};
        QueryPerformanceFrequency(&v);
        return v.QuadPart ? static_cast<double>(v.QuadPart) : 1.0;
    }();
    return frequency;
}

double TicksMs(uint64_t ticks)
{
    return 1000.0 * static_cast<double>(ticks) / QpcFrequency();
}

double Percentile(std::vector<uint32_t>& values, double fraction)
{
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    size_t index = static_cast<size_t>(std::floor(fraction * (values.size() - 1)));
    return static_cast<double>(values[index]);
}

uint32_t Maximum(std::vector<uint32_t> const& values)
{
    return values.empty() ? 0u : *std::max_element(values.begin(), values.end());
}

std::string JsonEscape(std::string const& value)
{
    std::ostringstream out;
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out << '\\'; out << static_cast<char>(c); }
        else if (c == '\b') out << "\\b";
        else if (c == '\f') out << "\\f";
        else if (c == '\n') out << "\\n";
        else if (c == '\r') out << "\\r";
        else if (c == '\t') out << "\\t";
        else if (c < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c) << std::dec;
        else out << static_cast<char>(c);
    }
    return out.str();
}

std::string HexBytes(unsigned char const* data, size_t size)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < size; ++i) out << std::setw(2) << static_cast<unsigned>(data[i]);
    return out.str();
}

struct RttSamples
{
    std::vector<uint32_t> values;
    void Add(uint64_t sent, uint64_t now)
    {
        if (now < sent) return;
        const double ms = TicksMs(now - sent);
        if (ms >= 0 && ms < 3600000.0)
            values.push_back(static_cast<uint32_t>(ms + 0.5));
    }
    std::string Json(char const* name) const
    {
        std::vector<uint32_t> copy = values;
        std::ostringstream out;
        out << '"' << name << "\":{" << "\"samples\":" << values.size()
            << ",\"p50_ms\":" << Percentile(copy, 0.50)
            << ",\"p95_ms\":" << Percentile(copy, 0.95)
            << ",\"max_ms\":" << Maximum(values) << '}';
        return out.str();
    }
};

static uint64_t ReadU64(unsigned char const* p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

static uint32_t ReadU32(unsigned char const* p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static void WriteU32(unsigned char* p, uint32_t v)
{
    p[0] = static_cast<unsigned char>(v);
    p[1] = static_cast<unsigned char>(v >> 8);
    p[2] = static_cast<unsigned char>(v >> 16);
    p[3] = static_cast<unsigned char>(v >> 24);
}

static void WriteU64(unsigned char* p, uint64_t v)
{
    for (unsigned i = 0; i < 8; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i));
}

static uint64_t EnvU64(char const* name, uint64_t fallback)
{
    char value[128] = {};
    if (!GetEnvironmentVariableA(name, value, sizeof(value))) return fallback;
    return std::strtoull(value, nullptr, 10);
}

struct CallbackObservation
{
    uint64_t callback504Count = 0;
    uint64_t callback505Count = 0;
    uint64_t callback1252Count = 0;
    uint64_t callback504Ticks = 0;
    uint64_t callback504Lobby = 0;
    bool callback504TagEmpty = true;
    bool callback504SessionEmpty = true;
    char callback504Tag[96] = {};
    char callback504Session[96] = {};
};

static std::mutex callbackObservationMutex;
static CallbackObservation callbackObservation;
static unsigned char(__cdecl* originalDispatchNext)(int, void*);
static decltype(g3t::steam::api.DispatchResult) originalDispatchResult;

static void ObserveCallback(int callback, char const* param, int size)
{
    if (callback == 1252) {
        std::lock_guard<std::mutex> lock(callbackObservationMutex);
        ++callbackObservation.callback1252Count;
        return;
    }
    if ((callback == 504 || callback == 505) && param && size >= 8) {
        uint64_t lobby = 0;
        std::memcpy(&lobby, param, sizeof(lobby));
        char const* tag = g3t::steam::api.GetLobbyData
            ? g3t::steam::api.GetLobbyData(g3t::steam::rt.matchmaking, lobby, "g3together")
            : "";
        char const* session = g3t::steam::api.GetLobbyData
            ? g3t::steam::api.GetLobbyData(g3t::steam::rt.matchmaking, lobby, "session_id")
            : "";
        std::lock_guard<std::mutex> lock(callbackObservationMutex);
        if (callback == 504) {
            ++callbackObservation.callback504Count;
            if (callbackObservation.callback504Count > 1) return;
            callbackObservation.callback504Ticks = QpcTicks();
            callbackObservation.callback504Lobby = lobby;
            callbackObservation.callback504TagEmpty =
                !tag || !tag[0];
            callbackObservation.callback504SessionEmpty =
                !session || !session[0];
            std::memset(callbackObservation.callback504Tag, 0,
                        sizeof(callbackObservation.callback504Tag));
            std::memset(callbackObservation.callback504Session, 0,
                        sizeof(callbackObservation.callback504Session));
            if (tag)
                std::snprintf(callbackObservation.callback504Tag,
                              sizeof(callbackObservation.callback504Tag),
                              "%s", tag);
            if (session)
                std::snprintf(callbackObservation.callback504Session,
                              sizeof(callbackObservation.callback504Session),
                              "%s", session);
        } else {
            ++callbackObservation.callback505Count;
        }
    }
}

static unsigned char __cdecl ObservedDispatchNext(int pipe, void* message)
{
    const unsigned char found = originalDispatchNext(pipe, message);
    if (found && message) {
        struct CallbackMsg { int user; int callback; char* param; int size; };
        auto const* msg = static_cast<CallbackMsg const*>(message);
        ObserveCallback(msg->callback, msg->param, msg->size);
    }
    return found;
}

static unsigned char __cdecl ObservedDispatchResult(
    int pipe, uint64_t call, void* result, int size, int callback, unsigned char* failed)
{
    const unsigned char found = originalDispatchResult(
        pipe, call, result, size, callback, failed);
    if (found && (!failed || !*failed))
        ObserveCallback(callback, static_cast<char const*>(result), size);
    return found;
}

static size_t RealNativeEnetCommandHeader(unsigned command)
{
    static constexpr size_t sizes[] = {
        0, 8, 48, 44, 8, 4, 6, 8, 24, 8, 12, 16, 24
    };
    return command < sizeof(sizes) / sizeof(sizes[0]) ? sizes[command] : 0;
}

static FARPROC WINAPI ObservedGetProcAddress(HMODULE module, LPCSTR name)
{
    FARPROC address = GetProcAddress(module, name);
    if (address && std::strcmp(name, "SteamAPI_ManualDispatch_GetNextCallback") == 0) {
        originalDispatchNext = reinterpret_cast<decltype(originalDispatchNext)>(address);
        return reinterpret_cast<FARPROC>(&ObservedDispatchNext);
    }
    if (address && std::strcmp(name, "SteamAPI_ManualDispatch_GetAPICallResult") == 0) {
        originalDispatchResult = reinterpret_cast<decltype(originalDispatchResult)>(address);
        return reinterpret_cast<FARPROC>(&ObservedDispatchResult);
    }
    return address;
}

static unsigned RealNativeEnetFirstCommand(
    unsigned char const* frame, size_t size)
{
    if (!frame || size < 2) return 0;
    const unsigned flags =
        (static_cast<unsigned>(frame[0]) << 8) |
        static_cast<unsigned>(frame[1]);
    const size_t header = (flags & 0x8000u) ? 4u : 2u;
    if (size <= header) return 0;
    if ((frame[0] & 0x40u) != 0)
        return 0;
    return frame[header] & ENET_PROTOCOL_COMMAND_MASK;
}

struct ProductQueueSnapshot
{
    size_t bytes = 0;
    size_t depth = 0;
    uint32_t oldestMs = 0;
    size_t peakBytes = 0;
    size_t peakDepth = 0;
    uint32_t peakAgeMs = 0;
    uint64_t plainBytes = 0;
    uint64_t wireBytes = 0;
};

static std::mutex queueObservationMutex;
static ProductQueueSnapshot queueObservation;

static DWORD WINAPI ObservedWorkerWait(DWORD count, HANDLE const* handles,
                                      BOOL all, DWORD timeout)
{
    ProductQueueSnapshot snapshot;
    for (auto const& remote : g3t::steam::remotes) {
        if (!remote.id) continue;
        snapshot.bytes += remote.pending.Bytes();
        snapshot.depth += remote.pending.Depth();
        snapshot.oldestMs = (std::max)(snapshot.oldestMs,
                                       remote.pending.OldestAge(g3t::steam::Now()));
        snapshot.peakBytes = (std::max)(snapshot.peakBytes,
                                        remote.pending.Bytes());
        snapshot.peakDepth = (std::max)(snapshot.peakDepth,
                                        remote.pending.Depth());
        snapshot.peakAgeMs = (std::max)(snapshot.peakAgeMs,
                                        remote.pending.OldestAge(g3t::steam::Now()));
        snapshot.plainBytes += remote.pending.counters.plainBytes;
        snapshot.wireBytes += remote.pending.counters.wireBytes;
    }
    {
        std::lock_guard<std::mutex> lock(queueObservationMutex);
        queueObservation = snapshot;
    }
    return WaitForMultipleObjects(count, handles, all, timeout);
}

static ProductQueueSnapshot ReadProductQueue()
{
    std::lock_guard<std::mutex> lock(queueObservationMutex);
    return queueObservation;
}

class NativeFrameDecoder
{
    void* coder_ = enet_range_coder_create();
public:
    ~NativeFrameDecoder() { if (coder_) enet_range_coder_destroy(coder_); }
    bool Decode(std::vector<unsigned char> const& frame,
                std::vector<unsigned char>& plain)
    {
        if (!coder_ || frame.size() < 2) return false;
        const size_t header = frame[0] & 0x80u ? 4 : 2;
        if (frame.size() <= header) return false;
        if (!(frame[0] & 0x40u)) {
            plain = frame;
            return true;
        }
        plain.resize(g3t::steam::RelayPayloadLimit);
        std::memcpy(plain.data(), frame.data(), header);
        const size_t length = enet_range_coder_decompress(
            coder_, frame.data() + header, frame.size() - header,
            plain.data() + header, plain.size() - header);
        if (!length) return false;
        plain.resize(header + length);
        plain[0] &= ~0x40u;
        return true;
    }
};
}

extern "C" int __cdecl main(int argc, char** argv)
{
    static_assert(sizeof(void*) == 4, "peer requires the Win32 x86 ABI");
    char realismFake[MAX_PATH] = {};
    const DWORD realismFakeLength = GetEnvironmentVariableA(
        "G3T_REALISM_FAKE_DLL", realismFake, sizeof(realismFake));
    if (!realismFakeLength || realismFakeLength >= sizeof(realismFake)) {
        std::fprintf(stderr,
                     "ERROR: G3T_REALISM_FAKE_DLL is required; refusing to use an inherited or real Steam DLL\n");
        return 2;
    }
    HMODULE markerModule = LoadLibraryExA(realismFake, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!markerModule) {
        std::fprintf(stderr, "ERROR: G3T_REALISM_FAKE_DLL could not be loaded: %s (Win32=%lu)\n",
                     realismFake, GetLastError());
        return 2;
    }
    typedef int (FAKE_STEAM_CDECL *GetAbiInfo)(FakeSteamAbiInfoV1*, uint32_t);
    typedef int (FAKE_STEAM_CDECL *GetStats)(uint64_t, FakeSteamStatsV1*, uint32_t);
    typedef int (FAKE_STEAM_CDECL *GetConfig)(FakeSteamConfigV1*, uint32_t);
    auto getAbiInfo = reinterpret_cast<GetAbiInfo>(
        GetProcAddress(markerModule, "FakeSteam_GetAbiInfoV1"));
    auto getStatsMarker = reinterpret_cast<GetStats>(
        GetProcAddress(markerModule, "FakeSteam_GetStatsV1"));
    auto getConfigMarker = reinterpret_cast<GetConfig>(
        GetProcAddress(markerModule, "FakeSteam_GetConfigV1"));
    if (!getAbiInfo || !getStatsMarker || !getConfigMarker) {
        std::fprintf(stderr,
                     "ERROR: G3T_REALISM_FAKE_DLL lacks the required FakeSteam V1 diagnostics exports\n");
        FreeLibrary(markerModule);
        return 2;
    }
    FakeSteamAbiInfoV1 abi{};
    if (getAbiInfo(&abi, sizeof(abi)) != 1 ||
        abi.structBytes != sizeof(abi) || abi.abiVersion != 1 ||
        abi.steamnetIdentityBytes != 136 || abi.steamnetIdentityIdOffset != 8 ||
        abi.steamnetMessageSenderOffset != 12) {
        std::fprintf(stderr, "ERROR: G3T_REALISM_FAKE_DLL did not provide the required ABI marker\n");
        FreeLibrary(markerModule);
        return 2;
    }
    if (!SetEnvironmentVariableA("G3T_STEAM_DLL", realismFake)) {
        std::fprintf(stderr, "ERROR: could not set G3T_STEAM_DLL to the explicitly validated fake DLL\n");
        FreeLibrary(markerModule);
        return 2;
    }

    if (enet_initialize() != 0) {
        std::fprintf(stderr, "ENet initialization failed\n");
        return 2;
    }

    Args args = ParseArgs(argc, argv);
    g3t::probeConfig.host = args.host;
    g3t::probeConfig.sessionIdValid = true;
    g3t::probeConfig.connectionAuto = false;
    g3t::probeConfig.networkBudgetKib = 256;
    g3t::probeConfig.sendRate = 30;
    std::memcpy(g3t::probeConfig.sessionId, args.session,
                (std::min)(sizeof(g3t::probeConfig.sessionId) - 1,
                           std::strlen(args.session)));
    g3t::probeConfig.sessionId[sizeof(g3t::probeConfig.sessionId) - 1] = '\0';
    g3t::probeConfig.steamLobby[0] = '\0';
    g3t::probeConfig.steamHost[0] = '\0';

    ENetAddress address{};
    enet_address_set_host(&address, "127.0.0.1");
    address.port = args.enetPort;
    char const* enetError = nullptr;
    ENetHost* enet = enet_host_create(&address, 32, 8, 0, 0);
    if (!enet) {
        enetError = "loopback ENet socket could not be created";
        std::fprintf(stderr, "ENet host failed: %s\n", enetError);
        enet_deinitialize();
        return 2;
    }
    if (enet_host_compress_with_range_coder(enet) != 0) {
        enet_host_destroy(enet);
        std::fprintf(stderr, "ENet host failed: range coder allocation failed\n");
        enet_deinitialize();
        return 2;
    }
    enet->mtu = 1200;
    enet_socket_set_option(enet->socket, ENET_SOCKOPT_RCVBUF, 4 * 1024 * 1024);
    enet_socket_set_option(enet->socket, ENET_SOCKOPT_SNDBUF, 4 * 1024 * 1024);

    ENetAddress bound{};
    enet_socket_get_address(enet->socket, &bound);
    const uint16_t enetPort = bound.port ? bound.port : enet->address.port;

    char fakeDll[MAX_PATH] = {};
    GetEnvironmentVariableA("G3T_STEAM_DLL", fakeDll, sizeof(fakeDll));
    bool fakeConfigured = fakeDll[0] != '\0';

    if (!g3t::steam::Start(args.host, enetPort)) {
        std::fprintf(stderr, "steam::Start failed: %s\n", g3t::steam::Error());
    }
    char fakeJoinFriend[128] = {}, fakeJoinConnect[512] = {};
    uint32_t fakeJoinDelay = 0;
    GetEnvironmentVariableA("G3T_FAKESTEAM_JOIN_FRIEND", fakeJoinFriend, sizeof(fakeJoinFriend));
    GetEnvironmentVariableA("G3T_FAKESTEAM_JOIN_CONNECT", fakeJoinConnect, sizeof(fakeJoinConnect));
    char fakeJoinDelayText[32] = {};
    if (GetEnvironmentVariableA("G3T_FAKESTEAM_JOIN_DELAY_MS", fakeJoinDelayText, sizeof(fakeJoinDelayText)))
        fakeJoinDelay = std::strtoul(fakeJoinDelayText, nullptr, 10);
    HMODULE fakeModule = fakeDll[0] ? LoadLibraryExA(fakeDll, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) : nullptr;
    typedef void (FAKE_STEAM_CDECL *InjectRichJoin)(uint64_t, char const*, uint32_t);
    InjectRichJoin injectRichJoin = fakeModule
        ? reinterpret_cast<InjectRichJoin>(GetProcAddress(fakeModule, "FakeSteam_InjectRichJoinV1"))
        : nullptr;
    bool rejoinInjected = false;
    uint32_t reconnectMs = 0;
    {
        char reconnectText[32] = {};
        if (GetEnvironmentVariableA("G3T_FAKESTEAM_RECONNECT_MS", reconnectText, sizeof(reconnectText)))
            reconnectMs = std::strtoul(reconnectText, nullptr, 10);
    }

    ENetPeer* peer = nullptr;
    bool enetConnectStarted = false;
    bool enetConnected = false;
    bool enetDisconnected = false;
    uint32_t enetConnectState = 0, enetEvent = 0, enetEventData = 0;
    uint64_t startedTicks = QpcTicks();
    uint64_t lobbyEnterTicks = 0, hostLinkTicks = 0, enetConnectTicks = 0;
    uint32_t linkLostCount = 0, rejoinCount = 0;
    uint64_t enetConnectCount = 0, steamLinkCount = 0;
    uint64_t offeredBytes = 0, receivedAppBytes = 0;
    uint64_t offeredPackets = 0, receivedPackets = 0;
    uint64_t stressPosePackets = 0;
    uint64_t stressPoseEntityKeys = 64;
    uint64_t stressPosePayloadBytes = 1200;
    uint64_t stressPoseBoneCount = 24;
    uint64_t exactReliableSent = 0, exactReliableReceived = 0, exactReliableMismatch = 0;
    RttSamples channel0Rtt, channel1Rtt, playerProbeRtt, recoveredCriticalRtt;
    std::vector<uint32_t> enetRtt;
    uint32_t rttMaximum = 0;
    uint64_t firstNativeConnectHeader = RealNativeEnetCommandHeader(ENET_PROTOCOL_COMMAND_CONNECT);
    uint64_t firstNativeVerifyHeader = RealNativeEnetCommandHeader(ENET_PROTOCOL_COMMAND_VERIFY_CONNECT);
    uint64_t connectPackets = 0, verifyPackets = 0;
    uint64_t nativeOutboundFrames = 0, nativeOutboundCompressedFrames = 0;
    uint64_t nativeOutboundConnectFrames = 0, nativeOutboundVerifyFrames = 0;
    uint64_t nativeOutboundCompressedConnectFrames = 0;
    uint64_t nativeOutboundCompressedVerifyFrames = 0;
    uint32_t nativeObservedConnectHeader = 0, nativeObservedVerifyHeader = 0;
    bool sawLobbyEnter = false;
    bool sawHostLink = false;
    bool leaveSent = false;
    bool sawLinkAtLeastOnce = false;
    uint32_t rejoinEligibleMs = 0;

    uint64_t stressAccumulated = 0;
    uint64_t stressSequence = 0;
    std::mt19937 stressRandom(0x47335431u);
    uint64_t channel0Sequence = 0, channel1Sequence = 0, exactSequence = 0;
    uint64_t lastStressMs = 0, lastChannel1Ms = 0, lastExactMs = 0, lastPlayerProbeMs = 0;
    bool playerProbeSent = false;
    uint64_t lastPollTicks = startedTicks;
    uint64_t stressStartedTicks = 0;
    uint64_t stressEndedTicks = 0;
    uint32_t stressDurationMs = 0;
    uint64_t recoveryPhaseStartTicks = 0;
    uint32_t runElapsedMs = 0;
    uint64_t endedTicks = startedTicks;
    uint32_t peakOutstanding = 0;
    std::vector<std::pair<uint32_t, uint64_t>> outstandingSamples;
    std::vector<std::pair<uint32_t, uint64_t>> productQueueBytesSamples;
    uint64_t previousPlainQueueBytes = 0, previousWireQueueBytes = 0;
    uint64_t cumulativePlainQueueBytes = 0, cumulativeWireQueueBytes = 0;
    size_t peakProductQueueBytes = 0, peakProductQueueDepth = 0;
    uint32_t peakProductQueueAgeMs = 0;
    NativeFrameDecoder nativeCodec;
    bool startReady = g3t::steam::Ready();

    while (true) {
        const uint64_t nowTicks = QpcTicks();
        const uint64_t elapsedTicks = nowTicks - startedTicks;
        const uint32_t elapsedMs = static_cast<uint32_t>(std::min<uint64_t>(elapsedTicks * 1000ULL / static_cast<uint64_t>(QpcFrequency()), UINT32_MAX));
        runElapsedMs = elapsedMs;
        const uint32_t pollElapsed = static_cast<uint32_t>((nowTicks - lastPollTicks) * 1000ULL / static_cast<uint64_t>(QpcFrequency()));

        g3t::steam::Pump();

        const uint64_t lobby = g3t::steam::LobbyId();
        if (lobby && !sawLobbyEnter) {
            sawLobbyEnter = true;
            lobbyEnterTicks = nowTicks;
        }

        if (args.host) {
            if (!g3t::steam::LinkedCount())
                sawHostLink = false;
            if (g3t::steam::LinkedCount() > 0 && !sawHostLink) {
                sawHostLink = true;
                hostLinkTicks = nowTicks;
                ++steamLinkCount;
            }
        } else if (g3t::steam::HostLinked() && !sawHostLink) {
            if (sawLinkAtLeastOnce && !recoveryPhaseStartTicks)
                recoveryPhaseStartTicks = nowTicks;
            sawHostLink = true;
            sawLinkAtLeastOnce = true;
            hostLinkTicks = nowTicks;
            ++steamLinkCount;
        }

        if (!args.host && sawHostLink && !g3t::steam::HostLinked()) {
            ++linkLostCount;
            if (peer && peer->state == ENET_PEER_STATE_CONNECTED) {
                enet_peer_disconnect(peer, 0);
            }
            peer = nullptr;
            enetConnectStarted = false;
            enetConnected = false;
            sawHostLink = false;
            if (rejoinEligibleMs == 0)
                rejoinEligibleMs = elapsedMs;
        }

        if (!args.host && g3t::steam::HostLinked() && !enetConnectStarted &&
            std::strcmp(args.scenario, "metadata") != 0) {
            enetConnectStarted = true;
            ENetAddress relay{};
            enet_address_set_host(&relay, "127.0.0.1");
            relay.port = g3t::steam::RelayPort();
            peer = enet_host_connect(enet, &relay, 8, 0);
            if (!peer) {
                enetConnectStarted = false;
            }
        }

        ENetEvent event{};
        if (enet_host_service(enet, &event, 1) > 0) {
            if (event.type == ENET_EVENT_TYPE_CONNECT) {
                peer = event.peer;
                enetConnected = true;
                ++enetConnectCount;
                if (enetConnectCount >= 2)
                    recoveryPhaseStartTicks = nowTicks;
                enetConnectTicks = nowTicks;
                enetConnectState = peer->state;
                event.peer->data = reinterpret_cast<void*>(1);
                if (!stressStartedTicks) {
                    stressStartedTicks = nowTicks;
                    stressAccumulated = 0;
                    lastStressMs = 0;
                }
            } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                unsigned char const* data = event.packet->data;
                size_t size = event.packet->dataLength;
                if (size >= 16) {
                    unsigned char tag = data[0];
                    uint32_t seq = ReadU32(data + 4);
                    uint64_t sent = ReadU64(data + 8);
                    if (tag == 39) {
                        if (!args.host) {
                            ++receivedPackets;
                            receivedAppBytes += size;
                        }
                    } else if (tag == 3 && data[1] == 0xC2) {
                        if (!args.host) {
                            unsigned char echo[64] = {};
                            echo[0] = 3;
                            echo[1] = 0xC3;
                            WriteU32(echo + 4, seq);
                            WriteU64(echo + 8, sent);
                            ENetPacket* packet = enet_packet_create(echo, sizeof(echo), ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
                            if (packet) enet_peer_send(event.peer, 0, packet);
                        }
                    } else if (tag == 3 && data[1] == 0xC3) {
                        if (args.host) playerProbeRtt.Add(sent, nowTicks);
                    } else if (tag == 0xA1) {
                        if (size >= 64) {
                            unsigned char echo[64];
                            std::memcpy(echo, data, 64);
                            echo[0] = 0xB1;
                            ENetPacket* packet = enet_packet_create(echo, sizeof(echo), ENET_PACKET_FLAG_RELIABLE);
                            if (packet) enet_peer_send(event.peer, 1, packet);
                            channel1Sequence = seq + 1;
                        }
                    } else if (tag == 0xB1) {
                        channel1Rtt.Add(sent, nowTicks);
                        if (recoveryPhaseStartTicks && sent >= recoveryPhaseStartTicks)
                            recoveredCriticalRtt.Add(sent, nowTicks);
                    } else if (tag == 0xD1) {
                        unsigned char echo[256];
                        if (size >= sizeof(echo)) std::memcpy(echo, data, sizeof(echo));
                        else std::memset(echo, 0, sizeof(echo));
                        echo[0] = 0xD2;
                        ENetPacket* packet = enet_packet_create(echo, sizeof(echo), ENET_PACKET_FLAG_RELIABLE);
                        if (packet) enet_peer_send(event.peer, 1, packet);
                        if (!args.host && size >= sizeof(echo)) {
                            ++exactReliableReceived;
                            unsigned char expected[256];
                            std::memset(expected, 0, sizeof(expected));
                            expected[0] = 0xD1;
                            const uint32_t seq1 = ReadU32(data + 4);
                            const uint64_t sent1 = ReadU64(data + 8);
                            WriteU32(expected + 4, seq1);
                            WriteU64(expected + 8, sent1);
                            for (size_t i = 16; i < sizeof(expected); ++i)
                                expected[i] = static_cast<unsigned char>((i * 37u + seq1) & 0xffu);
                            if (std::memcmp(expected, data, sizeof(expected)) != 0)
                                ++exactReliableMismatch;
                        }
                    } else if (tag == 0xD2) {
                        if (args.host && size >= 256) {
                            ++exactReliableReceived;
                            unsigned char expected[256];
                            std::memset(expected, 0, sizeof(expected));
                            expected[0] = 0xD2;
                            const uint32_t seq1 = ReadU32(data + 4);
                            const uint64_t sent1 = ReadU64(data + 8);
                            WriteU32(expected + 4, seq1);
                            WriteU64(expected + 8, sent1);
                            for (size_t i = 16; i < sizeof(expected); ++i)
                                expected[i] = static_cast<unsigned char>((i * 37u + seq1) & 0xffu);
                            if (std::memcmp(expected, data, sizeof(expected)) != 0)
                                ++exactReliableMismatch;
                        }
                    }
                }
                enet_packet_destroy(event.packet);
            } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                enetDisconnected = true;
                ++linkLostCount;
                enetEvent = static_cast<unsigned>(ENET_EVENT_TYPE_DISCONNECT);
                enetEventData = event.data;
                enetConnectState = event.peer ? event.peer->state : ENET_PEER_STATE_DISCONNECTED;
                if (peer == event.peer) {
                    peer = nullptr;
                    enetConnectStarted = false;
                    enetConnected = false;
                }
            }
        }
        enet_host_flush(enet);

        std::vector<std::vector<unsigned char>> outbound;
        {
            std::lock_guard<std::mutex> lock(nativeFrameCapture.mutex);
            outbound.swap(nativeFrameCapture.frames);
        }
        for (auto const& frame : outbound) {
            ++nativeOutboundFrames;
            const bool compressed = frame.size() >= 2 &&
                                    (frame[0] & 0x40u) != 0;
            nativeOutboundCompressedFrames += compressed ? 1 : 0;
            std::vector<unsigned char> plain;
            if (!nativeCodec.Decode(frame, plain))
                continue;
            const unsigned firstCommand = RealNativeEnetFirstCommand(
                plain.data(), plain.size());
            if (firstCommand == ENET_PROTOCOL_COMMAND_CONNECT) {
                ++nativeOutboundConnectFrames;
                nativeOutboundCompressedConnectFrames += compressed ? 1 : 0;
                nativeObservedConnectHeader =
                    static_cast<uint32_t>(
                        RealNativeEnetCommandHeader(ENET_PROTOCOL_COMMAND_CONNECT));
            }
            if (firstCommand == ENET_PROTOCOL_COMMAND_VERIFY_CONNECT) {
                ++nativeOutboundVerifyFrames;
                nativeOutboundCompressedVerifyFrames += compressed ? 1 : 0;
                nativeObservedVerifyHeader =
                    static_cast<uint32_t>(
                        RealNativeEnetCommandHeader(ENET_PROTOCOL_COMMAND_VERIFY_CONNECT));
            }
        }

        if (peer && peer->state == ENET_PEER_STATE_CONNECTED) {
            enetConnected = true;
            rttMaximum = (std::max)(rttMaximum, static_cast<uint32_t>(peer->roundTripTime));
            enetRtt.push_back(static_cast<uint32_t>(peer->roundTripTime));

            if (args.host) {
                const uint64_t activeTicks = stressStartedTicks ? nowTicks - stressStartedTicks : 0;
                const uint64_t nowMs = activeTicks * 1000ULL / static_cast<uint64_t>(QpcFrequency());
                if (stressStartedTicks && nowMs - lastStressMs >= 1) {
                    const uint64_t delta = nowMs - lastStressMs;
                    lastStressMs = static_cast<uint32_t>(nowMs);
                    stressAccumulated += args.appBps * delta;
                    while (stressAccumulated >= 1200u * 1000u) {
                        unsigned char payload[1200];
                        std::memset(payload, 0, sizeof(payload));

                    const uint64_t entityIndex =
                        (stressSequence++ % stressPoseEntityKeys);
                    uint64_t key = 0x1000000000000000ULL |
                                   (entityIndex + 1);

                    payload[0] = 39; // MsgPosedNpc: recognized full-hash NPC pose
                    uint32_t sequence = static_cast<uint32_t>(++channel0Sequence);
                    uint32_t sent = static_cast<uint32_t>(nowTicks & 0xffffffffu);
                    uint32_t epoch = 1;
                    uint32_t generation = 1;
                    uint32_t publication = sequence;
                    std::memcpy(payload + 1, &sequence, 4);
                    std::memcpy(payload + 5, &sent, 4);
                    std::memcpy(payload + 9, &epoch, 4);
                    std::memcpy(payload + 13, &generation, 4);
                    std::memcpy(payload + 17, &publication, 4);
                    payload[21] = 1;
                    payload[22] = 1;

                    g3t::net::NpcRecord record{};
                    for (unsigned i = 0; i < 16; ++i)
                        record.guid[i] = static_cast<uint8_t>(
                            (key >> (8 * (i & 7))) ^ (i * 37u));
                    record.position[0] = static_cast<float>(
                        (entityIndex % 17) * 100.0f);
                    record.position[1] = 0.0f;
                    record.position[2] = static_cast<float>(
                        (entityIndex % 29) * 100.0f);
                    record.flags = 0;
                    record.incarnation = 1;
                    record.partyPlayer = static_cast<uint8_t>(
                        (entityIndex % 7) + 1);
                    record.sent = sent;

                    std::memcpy(payload + 23, &record, sizeof(record));
                    size_t at = 23 + sizeof(record);
                    uint16_t boneCount = static_cast<uint16_t>(stressPoseBoneCount);
                    std::memcpy(payload + at, &boneCount, 2);
                    at += 2;
                    payload[at++] = 0; // pose flags
                    payload[at++] = 16; // intervalMs

                    for (uint16_t i = 0; i < boneCount && at + 28 <= sizeof(payload); ++i) {
                        uint32_t boneName = 0x10000000u +
                                            static_cast<uint32_t>(i + 1);
                        std::memcpy(payload + at, &boneName, 4);
                        at += 4;
                        for (int k = 0; k < 12; ++k) {
                            int16_t value = 0;
                            if (std::strcmp(args.profile, "compressible") != 0) {
                                value = static_cast<int16_t>(
                                    stressRandom() & 0xffffu);
                            }
                            std::memcpy(payload + at, &value, 2);
                            at += 2;
                        }
                    }

                    for (; at < sizeof(payload); ++at) {
                        if (std::strcmp(args.profile, "compressible") == 0)
                            payload[at] = 0;
                        else {
                            payload[at] = static_cast<unsigned char>(
                                stressRandom() & 0xffu);
                        }
                    }
                    if (std::strcmp(args.profile, "compressible") != 0) {
                        for (size_t i = 1; i < sizeof(payload); ++i)
                            payload[i] = static_cast<unsigned char>(
                                stressRandom() & 0xffu);
                    }
                    ENetPacket* packet = enet_packet_create(payload, sizeof(payload),
                                                           ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
                        if (packet && enet_peer_send(peer, 4, packet) == 0) {
                            ++offeredPackets;
                            ++stressPosePackets;
                            offeredBytes += sizeof(payload);
                        } else if (packet) enet_packet_destroy(packet);
                        stressAccumulated -= 1200u * 1000u;
                    }
                }

                if (!playerProbeSent) {
                    playerProbeSent = true;
                    lastPlayerProbeMs = static_cast<uint32_t>(nowMs);
                    unsigned char payload[120] = {};
                    payload[0] = 3;
                    payload[1] = 0xC2;
                    WriteU32(payload + 4, static_cast<uint32_t>(++channel0Sequence));
                    WriteU64(payload + 8, nowTicks);
                    ENetPacket* packet = enet_packet_create(
                        payload, sizeof(payload),
                        ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
                    if (packet) enet_peer_send(peer, 0, packet);
                } else if (nowMs - lastPlayerProbeMs >= 500) {
                    lastPlayerProbeMs = static_cast<uint32_t>(nowMs);
                    unsigned char payload[120] = {};
                    payload[0] = 3;
                    payload[1] = 0xC2;
                    WriteU32(payload + 4, static_cast<uint32_t>(++channel0Sequence));
                    WriteU64(payload + 8, nowTicks);
                    ENetPacket* packet = enet_packet_create(
                        payload, sizeof(payload),
                        ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
                    if (packet) enet_peer_send(peer, 0, packet);
                }

                if (nowMs - lastChannel1Ms >= 100) {
                    lastChannel1Ms = nowMs;
                    unsigned char payload[64];
                    std::memset(payload, 0x31, sizeof(payload));
                    payload[0] = 0xA1;
                    WriteU32(payload + 4, static_cast<uint32_t>(++channel1Sequence));
                    WriteU64(payload + 8, nowTicks);
                    ENetPacket* packet = enet_packet_create(payload, sizeof(payload), ENET_PACKET_FLAG_RELIABLE);
                    if (packet) enet_peer_send(peer, 1, packet);
                }

                if (nowMs - lastExactMs >= 500) {
                    lastExactMs = nowMs;
                    unsigned char payload[256];
                    std::memset(payload, 0, sizeof(payload));
                    payload[0] = 0xD1;
                    WriteU32(payload + 4, static_cast<uint32_t>(++exactSequence));
                    WriteU64(payload + 8, nowTicks);
                    for (size_t i = 16; i < sizeof(payload); ++i)
                        payload[i] = static_cast<unsigned char>((i * 37u + exactSequence) & 0xffu);
                    ENetPacket* packet = enet_packet_create(payload, sizeof(payload), ENET_PACKET_FLAG_RELIABLE);
                    if (packet) {
                        ++exactReliableSent;
                        enet_peer_send(peer, 1, packet);
                    }
                }
            }

            if (!args.host && pollElapsed >= 1 && elapsedMs - lastChannel1Ms >= 100) {
                lastChannel1Ms = elapsedMs;
                unsigned char payload[64];
                std::memset(payload, 0x32, sizeof(payload));
                payload[0] = 0xA1;
                WriteU32(payload + 4, static_cast<uint32_t>(++channel1Sequence));
                WriteU64(payload + 8, nowTicks);
                ENetPacket* packet = enet_packet_create(payload, sizeof(payload), ENET_PACKET_FLAG_RELIABLE);
                if (packet) enet_peer_send(peer, 1, packet);
            }
        }

        if (peer && args.leaveMs && elapsedMs >= args.leaveMs && !leaveSent) {
            leaveSent = true;
            g3t::steam::RequestClientDisconnect();
            enet_peer_disconnect(peer, 0);
        }
        const bool recoveryScenario =
            std::strcmp(args.scenario, "rejoin") == 0 ||
            std::strcmp(args.scenario, "bad") == 0;
        const uint32_t recoveryDelayMs =
            std::strcmp(args.scenario, "bad") == 0 ? 3500 : 1000;
        if (!args.host && recoveryScenario &&
            rejoinEligibleMs &&
            elapsedMs >= rejoinEligibleMs + recoveryDelayMs &&
            !rejoinInjected && injectRichJoin &&
            fakeJoinFriend[0] && fakeJoinConnect[0]) {
            injectRichJoin(std::strtoull(fakeJoinFriend, nullptr, 10), fakeJoinConnect, 0);
            rejoinInjected = true;
            ++rejoinCount;
            recoveryPhaseStartTicks = nowTicks;
        }

        const uint64_t outstanding = offeredBytes > receivedAppBytes ? offeredBytes - receivedAppBytes : 0;
        if (outstanding > peakOutstanding) peakOutstanding = static_cast<uint32_t>(std::min<uint64_t>(outstanding, UINT32_MAX));
        if (outstandingSamples.empty() || elapsedMs - outstandingSamples.back().first >= 100)
            outstandingSamples.push_back({elapsedMs, outstanding});

        if (productQueueBytesSamples.empty() ||
            elapsedMs - productQueueBytesSamples.back().first >= 100) {
            ProductQueueSnapshot queue = ReadProductQueue();
            peakProductQueueBytes = (std::max)(peakProductQueueBytes, queue.peakBytes);
            peakProductQueueDepth = (std::max)(peakProductQueueDepth, queue.peakDepth);
            peakProductQueueAgeMs = (std::max)(peakProductQueueAgeMs, queue.peakAgeMs);
            if (queue.plainBytes >= previousPlainQueueBytes)
                cumulativePlainQueueBytes += queue.plainBytes - previousPlainQueueBytes;
            else
                cumulativePlainQueueBytes += queue.plainBytes;
            if (queue.wireBytes >= previousWireQueueBytes)
                cumulativeWireQueueBytes += queue.wireBytes - previousWireQueueBytes;
            else
                cumulativeWireQueueBytes += queue.wireBytes;
            previousPlainQueueBytes = queue.plainBytes;
            previousWireQueueBytes = queue.wireBytes;
            if (productQueueBytesSamples.empty() ||
                elapsedMs - productQueueBytesSamples.back().first >= 100)
                productQueueBytesSamples.push_back({elapsedMs, queue.bytes});
        }

        const uint64_t activeTicks = stressStartedTicks ? nowTicks - stressStartedTicks : elapsedTicks;
        const uint32_t activeElapsedMs = static_cast<uint32_t>(
            std::min<uint64_t>(activeTicks * 1000ULL /
                               static_cast<uint64_t>(QpcFrequency()), UINT32_MAX));
        runElapsedMs = activeElapsedMs;
        const bool connectionTimedOut =
            std::strcmp(args.scenario, "metadata") != 0 &&
            !stressStartedTicks && elapsedMs >= 10000;
        if (activeElapsedMs >= args.seconds * 1000u || connectionTimedOut) {
            if (args.host && stressStartedTicks)
                stressDurationMs = activeElapsedMs;
            for (unsigned i = 0; i < 10 && leaveSent; ++i) {
                if (enet_host_service(enet, &event, 10) > 0 && event.type == ENET_EVENT_TYPE_DISCONNECT) break;
                enet_host_flush(enet);
            }
            endedTicks = nowTicks;
            break;
        }
    }

    ProductQueueSnapshot finalQueue = ReadProductQueue();
    peakProductQueueBytes = (std::max)(peakProductQueueBytes, finalQueue.peakBytes);
    peakProductQueueDepth = (std::max)(peakProductQueueDepth, finalQueue.peakDepth);
    peakProductQueueAgeMs = (std::max)(peakProductQueueAgeMs, finalQueue.peakAgeMs);
    const uint64_t plainTx = cumulativePlainQueueBytes;
    const uint64_t wireTx = cumulativeWireQueueBytes;
    const double compressionRatio = wireTx ? static_cast<double>(plainTx) / static_cast<double>(wireTx) : 0.0;
    double productQueueSlope = 0.0;
    if (productQueueBytesSamples.size() >= 2) {
        const double n = static_cast<double>(productQueueBytesSamples.size());
        double sx = 0.0, sy = 0.0, sxy = 0.0, sx2 = 0.0;
        for (auto const& sample : productQueueBytesSamples) {
            sx += sample.first;
            sy += sample.second;
            sxy += static_cast<double>(sample.first) * sample.second;
            sx2 += static_cast<double>(sample.first) * sample.first;
        }
        const double denominator = n * sx2 - sx * sx;
        if (denominator != 0.0)
            productQueueSlope = (n * sxy - sx * sy) / denominator * 1000.0;
    }

    g3t::steam::TrafficCounters traffic = g3t::steam::Traffic();
    CallbackObservation callbackSnapshot;
    {
        std::lock_guard<std::mutex> lock(callbackObservationMutex);
        callbackSnapshot = callbackObservation;
    }

    const double lobbyDelayMs = callbackSnapshot.callback504Ticks &&
        hostLinkTicks >= callbackSnapshot.callback504Ticks
        ? TicksMs(hostLinkTicks - callbackSnapshot.callback504Ticks) : 0.0;
    const double enetConnectMs = sawHostLink && enetConnectTicks
        ? TicksMs(enetConnectTicks - startedTicks) : 0.0;

    double slope = 0.0;
    if (outstandingSamples.size() >= 2) {
        const double n = static_cast<double>(outstandingSamples.size());
        double sx = 0.0, sy = 0.0, sxy = 0.0, sx2 = 0.0;
        for (auto const& sample : outstandingSamples) {
            sx += sample.first;
            sy += sample.second;
            sxy += static_cast<double>(sample.first) * sample.second;
            sx2 += static_cast<double>(sample.first) * sample.first;
        }
        const double denominator = n * sx2 - sx * sx;
        if (denominator != 0.0) slope = (n * sxy - sx * sy) / denominator * 1000.0;
    }

    FakeSteamStatsV1 fakeStats{};
    FakeSteamConfigV1 fakeConfig{};
    bool fakeStatsAvailable = false;
    bool fakeConfigAvailable = false;
    int fakeStatsAbiReturn = -1;
    int fakeConfigAbiReturn = -1;
    if (markerModule) {
        typedef int (FAKE_STEAM_CDECL *GetStatsV1)(uint64_t, FakeSteamStatsV1*, uint32_t);
        typedef int (FAKE_STEAM_CDECL *GetConfigV1)(FakeSteamConfigV1*, uint32_t);
        auto getStats = reinterpret_cast<GetStatsV1>(
            GetProcAddress(markerModule, "FakeSteam_GetStatsV1"));
        auto getConfig = reinterpret_cast<GetConfigV1>(
            GetProcAddress(markerModule, "FakeSteam_GetConfigV1"));
        if (getStats &&
            (fakeStatsAbiReturn = getStats(0, &fakeStats, sizeof(fakeStats))) == 1 &&
            fakeStats.abiVersion == 1 &&
            fakeStats.structBytes == sizeof(fakeStats))
            fakeStatsAvailable = true;
        if (getConfig &&
            (fakeConfigAbiReturn = getConfig(&fakeConfig, sizeof(fakeConfig))) == 1 &&
            fakeConfig.abiVersion == 1 &&
            fakeConfig.structBytes == sizeof(fakeConfig))
            fakeConfigAvailable = true;
    }
    const uint32_t finalLinkedCount = g3t::steam::LinkedCount();
    const uint32_t finalPing = g3t::steam::Ping();
    const std::string finalError = g3t::steam::Error();
    g3t::steam::Stop();
    enet_host_destroy(enet);
    enet_deinitialize();

    const uint64_t runSeconds = args.seconds;
    std::ostringstream json;
    json << '{'
         << "\"schema\":1"
         << ",\"role\":\"" << JsonEscape(args.role) << '\"'
         << ",\"profile\":\"" << JsonEscape(args.profile) << '\"'
         << ",\"source_root\":\"" << JsonEscape(args.sourceRoot) << '\"'
         << ",\"session\":\"" << JsonEscape(args.session) << '\"'
         << ",\"seconds\":" << runSeconds
         << ",\"app_bps_requested\":" << args.appBps
         << ",\"physical_bps_requested\":" << args.physicalBps
         << ",\"stress_offer_started\":" << (stressStartedTicks ? "true" : "false")
         << ",\"stress_offer_duration_ms\":" << stressDurationMs
         << ",\"stress_offer_bps_observed\":" << (stressDurationMs
             ? static_cast<uint64_t>(offeredBytes * 1000ULL / stressDurationMs)
             : 0ULL)
         << ",\"host_id\":" << args.hostId
         << ",\"client_id\":" << args.clientId
         << ",\"seeded_bypass\":" << (args.seeded ? "true" : "false")
         << ",\"enet_port\":" << enetPort
         << ",\"fake_configured\":" << (fakeConfigured ? "true" : "false")
         << ",\"ready\":" << (startReady ? "true" : "false")
         << ",\"lobby_enter_observed\":" << (sawLobbyEnter ? "true" : "false")
         << ",\"host_link_observed\":" << (sawHostLink ? "true" : "false")
         << ",\"lobby_enter_ms\":" << (sawLobbyEnter ? TicksMs(lobbyEnterTicks - startedTicks) : 0.0)
         << ",\"host_link_ms\":" << (sawHostLink ? TicksMs(hostLinkTicks - startedTicks) : 0.0)
         << ",\"lobby_to_link_ms\":" << lobbyDelayMs
         << ",\"callback504_count_actual\":" << callbackSnapshot.callback504Count
         << ",\"callback504_lobby\":" << callbackSnapshot.callback504Lobby
         << ",\"callback504_delivered_ms\":" << (callbackSnapshot.callback504Ticks
             ? TicksMs(callbackSnapshot.callback504Ticks - startedTicks) : 0.0)
         << ",\"callback504_tag_empty\":" << (callbackSnapshot.callback504TagEmpty ? "true" : "false")
         << ",\"callback504_session_empty\":" << (callbackSnapshot.callback504SessionEmpty ? "true" : "false")
         << ",\"callback504_tag\":\"" << JsonEscape(callbackSnapshot.callback504Tag) << '\"'
         << ",\"callback504_session\":\"" << JsonEscape(callbackSnapshot.callback504Session) << '\"'
         << ",\"callback505_count_actual\":" << callbackSnapshot.callback505Count
         << ",\"session_failed_callbacks\":" << callbackSnapshot.callback1252Count
         << ",\"lobby_enter_callback_observed\":" << (callbackSnapshot.callback504Count > 0 ? "true" : "false")
         << ",\"metadata_empty_at_lobby_enter\":" << (callbackSnapshot.callback504Count > 0 &&
                                                         callbackSnapshot.callback504TagEmpty &&
                                                         callbackSnapshot.callback504SessionEmpty ? "true" : "false")
         << ",\"metadata_update_callback_observed\":" << (callbackSnapshot.callback505Count > 0 ? "true" : "false")
         << ",\"enet_connect_count\":" << enetConnectCount
         << ",\"steam_link_count\":" << steamLinkCount
         << ",\"recovered_echo_samples\":" << recoveredCriticalRtt.values.size()
         << ",\"enet_connected\":" << (enetConnected ? "true" : "false")
         << ",\"enet_connect_ms\":" << enetConnectMs
         << ",\"enet_disconnected\":" << (enetDisconnected ? "true" : "false")
         << ",\"linked_count\":" << finalLinkedCount
         << ",\"steam_ping_ms\":" << finalPing
         << ",\"link_lost_count\":" << linkLostCount
         << ",\"rejoin_count\":" << rejoinCount
         << ",\"native_connect_header_size\":" << firstNativeConnectHeader
         << ",\"native_verify_header_size\":" << firstNativeVerifyHeader
         << ",\"connect_packets_observed\":" << connectPackets
         << ",\"verify_packets_observed\":" << verifyPackets
         << ",\"native_outbound_frames\":" << nativeOutboundFrames
         << ",\"native_outbound_compressed_frames\":" << nativeOutboundCompressedFrames
         << ",\"native_outbound_connect_frames\":" << nativeOutboundConnectFrames
         << ",\"native_outbound_verify_frames\":" << nativeOutboundVerifyFrames
         << ",\"native_outbound_compressed_connect_frames\":" << nativeOutboundCompressedConnectFrames
         << ",\"native_outbound_compressed_verify_frames\":" << nativeOutboundCompressedVerifyFrames
         << ",\"native_observed_connect_header\":" << nativeObservedConnectHeader
         << ",\"native_observed_verify_header\":" << nativeObservedVerifyHeader
         << ",\"channel0_app_offered_bytes\":" << offeredBytes
         << ",\"channel0_app_received_bytes\":" << receivedAppBytes
         << ",\"channel0_app_offered_packets\":" << offeredPackets
         << ",\"channel0_app_received_packets\":" << receivedPackets
         << ",\"stress_pose_packets\":" << stressPosePackets
         << ",\"stress_pose_opcode\":" << 39
         << ",\"stress_pose_layout\":\""
         << (std::strcmp(args.profile, "compressible") == 0
                 ? "full-hash-NpcRecord" : "opaque-pose-priority-load")
         << "\""
         << ",\"stress_pose_channel\":" << 4
         << ",\"stress_pose_entity_population\":"
         << (std::strcmp(args.profile, "compressible") == 0 ? stressPoseEntityKeys : 0)
         << ",\"stress_pose_bone_count\":"
         << (std::strcmp(args.profile, "compressible") == 0 ? stressPoseBoneCount : 0)
         << ",\"stress_pose_payload_bytes\":" << stressPosePayloadBytes
         << ",\"channel0_outstanding_bytes\":" << (offeredBytes > receivedAppBytes ? offeredBytes - receivedAppBytes : 0)
         << ",\"channel0_peak_outstanding_bytes\":" << peakOutstanding
         << ",\"channel0_outstanding_slope_bytes_per_second\":" << slope
         << ",\"product_queue_bytes\":" << finalQueue.bytes
         << ",\"product_queue_depth\":" << finalQueue.depth
         << ",\"product_queue_oldest_ms\":" << finalQueue.oldestMs
         << ",\"product_queue_peak_bytes\":" << peakProductQueueBytes
         << ",\"product_queue_peak_depth\":" << peakProductQueueDepth
         << ",\"product_queue_peak_age_ms\":" << peakProductQueueAgeMs
         << ",\"product_queue_slope_bytes_per_second\":" << productQueueSlope
         << ",\"exact_reliable_payloads_sent\":" << exactReliableSent
         << ",\"exact_reliable_payloads_received\":" << exactReliableReceived
         << ",\"exact_reliable_payload_mismatch\":" << exactReliableMismatch
         << ",\"rtt_enet_p50_ms\":" << Percentile(enetRtt, 0.50)
         << ",\"rtt_enet_p95_ms\":" << Percentile(enetRtt, 0.95)
         << ",\"rtt_enet_max_ms\":" << rttMaximum
         << ",\"rtt_enet_samples\":" << enetRtt.size()
         << ',' << channel0Rtt.Json("channel0_echo")
         << ',' << channel1Rtt.Json("channel1_echo")
         << ',' << playerProbeRtt.Json("player_opcode3_echo")
         << ",\"steam_plain_tx_bytes\":" << plainTx
         << ",\"steam_wire_tx_bytes\":" << wireTx
         << ",\"compression_ratio_plain_over_wire\":" << compressionRatio
         << ",\"steam_tx_datagrams\":" << traffic.txDatagrams
         << ",\"steam_rx_datagrams\":" << traffic.rxDatagrams
         << ",\"fake_stats_available\":" << (fakeStatsAvailable ? "true" : "false")
         << ",\"fake_stats_abi_return\":" << fakeStatsAbiReturn
         << ",\"fake_stats_struct_bytes_received\":" << fakeStats.structBytes
         << ",\"fake_stats_struct_bytes_expected\":" << sizeof(fakeStats)
         << ",\"fake_stats_abi_version_received\":" << fakeStats.abiVersion
         << ",\"fake_stats\":" << (fakeStatsAvailable
             ? ("{\"pending_bytes\":" + std::to_string(fakeStats.pendingBytes) +
                ",\"max_pending\":" + std::to_string(fakeStats.maxPending) +
                ",\"pending_reliable\":" + std::to_string(fakeStats.pendingReliable) +
                ",\"pending_unreliable\":" + std::to_string(fakeStats.pendingUnreliable) +
                ",\"sent_unacked\":" + std::to_string(fakeStats.sentUnacked) +
                ",\"accepted_messages\":" + std::to_string(fakeStats.acceptedMessages) +
                ",\"accepted_bytes\":" + std::to_string(fakeStats.acceptedBytes) +
                ",\"result25_count\":" + std::to_string(fakeStats.result25Count) +
                ",\"result41_count\":" + std::to_string(fakeStats.result41Count) +
                ",\"wire_bytes\":" + std::to_string(fakeStats.wireBytes) +
                ",\"wire_packets\":" + std::to_string(fakeStats.wirePackets) +
                ",\"delivered_bytes\":" + std::to_string(fakeStats.deliveredBytes) +
                ",\"delivered_messages\":" + std::to_string(fakeStats.deliveredMessages) +
                ",\"dropped_data_packets\":" + std::to_string(fakeStats.droppedDataPackets) +
                ",\"dropped_ack_packets\":" + std::to_string(fakeStats.droppedAckPackets) +
                ",\"reliable_retries\":" + std::to_string(fakeStats.reliableRetries) +
                ",\"duplicate_messages\":" + std::to_string(fakeStats.duplicateMessages) +
                ",\"session_starts\":" + std::to_string(fakeStats.sessionStarts) +
                ",\"session_failures\":" + std::to_string(fakeStats.sessionFailures) +
                ",\"session_restarts\":" + std::to_string(fakeStats.sessionRestarts) +
                ",\"current_queue_age_ms\":" + std::to_string(fakeStats.currentQueueAgeMs) +
                ",\"max_queue_age_ms\":" + std::to_string(fakeStats.maxQueueAgeMs) +
                ",\"callback333_count\":" + std::to_string(fakeStats.callback333Count) +
                ",\"callback337_count\":" + std::to_string(fakeStats.callback337Count) +
                ",\"callback504_count\":" + std::to_string(fakeStats.callback504Count) +
                ",\"callback505_count\":" + std::to_string(fakeStats.callback505Count) +
                ",\"callback506_count\":" + std::to_string(fakeStats.callback506Count) +
                ",\"callback703_count\":" + std::to_string(fakeStats.callback703Count) +
                ",\"callback1251_count\":" + std::to_string(fakeStats.callback1251Count) +
                ",\"callback1252_count\":" + std::to_string(fakeStats.callback1252Count) +
                ",\"requested_nodelay_count\":" + std::to_string(fakeStats.requestedNoDelayCount) +
                ",\"effective_reliable_count\":" + std::to_string(fakeStats.effectiveReliableCount) +
                ",\"queue_delay_events\":" + std::to_string(fakeStats.queueDelayEvents) +
                ",\"max_queue_delay_ms\":" + std::to_string(fakeStats.maxQueueDelayMs) +
                "}")
             : "{\"available\":false}")
         << ",\"fake_config_available\":" << (fakeConfigAvailable ? "true" : "false")
         << ",\"fake_config_abi_return\":" << fakeConfigAbiReturn
         << ",\"fake_config_struct_bytes_received\":" << fakeConfig.structBytes
         << ",\"fake_config_struct_bytes_expected\":" << sizeof(fakeConfig)
         << ",\"fake_config\":" << (fakeConfigAvailable
             ? ("{\"profile\":\"" + JsonEscape(fakeConfig.profile) +
                "\",\"physical_rate_bps\":" + std::to_string(fakeConfig.physicalRateBps) +
                ",\"latency_ms\":" + std::to_string(fakeConfig.latencyMs) +
                ",\"jitter_ms\":" + std::to_string(fakeConfig.jitterMs) +
                ",\"loss_permille\":" + std::to_string(fakeConfig.lossPermille) +
                ",\"ack_loss_permille\":" + std::to_string(fakeConfig.ackLossPermille) +
                ",\"setup_ms\":" + std::to_string(fakeConfig.setupMs) +
                ",\"metadata_delay_ms\":" + std::to_string(fakeConfig.metadataDelayMs) +
                ",\"reconnect_ms\":" + std::to_string(fakeConfig.reconnectMs) +
                ",\"outage_ms\":" + std::to_string(fakeConfig.outageMs) +
                ",\"nodelay_policy\":" + std::to_string(fakeConfig.nodelayPolicy) +
                ",\"queue_delay_ms\":" + std::to_string(fakeConfig.queueDelayMs) +
                ",\"queue_delay_period_ms\":" + std::to_string(fakeConfig.queueDelayPeriodMs) +
                ",\"queue_delay_window_ms\":" + std::to_string(fakeConfig.queueDelayWindowMs) +
                ",\"queue_delay_phase_ms\":" + std::to_string(fakeConfig.queueDelayPhaseMs) +
                ",\"queue_delay_enabled\":" + std::to_string(fakeConfig.queueDelayEnabled) +
                "}")
             : "{\"available\":false}")
         << ",\"sampled_before_stop\":true"
         << ",\"queue_sample_thread\":\"production-worker-before-native-wait\""
         << ",\"error\":\"" << JsonEscape(finalError.c_str()) << '\"'
         << '}';

    if (args.out[0]) {
        std::ofstream file(args.out, std::ios::binary | std::ios::trunc);
        file << json.str() << '\n';
        if (!file) {
            std::fprintf(stderr, "ERROR: structured metrics could not be written: %s\n", args.out);
            return 1;
        }
    }
    std::cout << json.str() << '\n';
    return fakeConfigured && startReady ? 0 : (fakeConfigured ? 1 : 0);
}
