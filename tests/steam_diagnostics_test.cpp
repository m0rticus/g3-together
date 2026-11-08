// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/steam_diagnostics.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace g3t::steam::diagnostics;

static void FlatApiLayout()
{
    ConnectionInfo info;
    auto* bytes = reinterpret_cast<unsigned char*>(&info);
    const int32_t state = Connected, flags = Relayed;
    const uint32_t pop = 0x00736561;
    std::memcpy(bytes + 172, &pop, sizeof(pop));
    std::memcpy(bytes + 176, &state, sizeof(state));
    std::memcpy(bytes + 312, "P2P SDR", 8);
    std::memcpy(bytes + 440, &flags, sizeof(flags));
    SessionStatus status;
    auto* quick = reinterpret_cast<unsigned char*>(&status);
    const int32_t rate = 262144;
    const int64_t queue = 2000000;
    std::memcpy(quick, &state, sizeof(state));
    std::memcpy(quick + 32, &rate, sizeof(rate));
    std::memcpy(quick + 48, &queue, sizeof(queue));
    assert(Current(state, info, status));
    assert(std::strcmp(Transport(state, info, status), "relay-sdr") == 0);
    assert(std::strcmp(info.description, "P2P SDR") == 0);
    assert(status.sendRate == rate && status.queueMicros == queue);
}

static void RouteEvidence()
{
    ConnectionInfo info;
    SessionStatus status;
    for (int state : {0, 1, 2, 4, 5}) {
        info.state = status.state = state;
        info.flags = Relayed;
        info.popRelay = 1;
        assert(!Current(state, info, status));
        assert(std::strcmp(Transport(state, info, status), "unknown") == 0);
    }
    info.state = status.state = Connected;
    assert(std::strcmp(Transport(Connected, info, status), "relay-sdr") == 0);
    info.popRelay = 0;
    assert(std::strcmp(Transport(Connected, info, status), "unknown") == 0);
    assert(std::strcmp(Evidence(Connected, info, status), "relayed-without-sdr-pop") == 0);
    info.flags = 0;
    assert(std::strcmp(Transport(Connected, info, status), "unknown") == 0);
    info.remoteAddress[16] = 1; // A port alone is not a peer address.
    assert(!HasRemoteAddress(info));
    info.remoteAddress[10] = info.remoteAddress[11] = 255;
    info.remoteAddress[12] = 192;
    info.remoteAddress[15] = 1;
    assert(std::strcmp(Transport(Connected, info, status), "direct-ice") == 0);
    info.popRelay = 1;
    assert(std::strcmp(Transport(Connected, info, status), "unknown") == 0);
    info.popRelay = 0;
    info.flags = LoopbackBuffers;
    assert(std::strcmp(Transport(Connected, info, status), "unknown") == 0);
    info.flags = 0;
    std::memset(info.remoteAddress, 0, sizeof(info.remoteAddress));
    info.remoteAddress[0] = 0x20; // Native IPv6 ICE route.
    assert(std::strcmp(Transport(Connected, info, status), "direct-ice") == 0);
    status.state = 0; // Old stand-ins that leave output structures untouched are not evidence.
    assert(std::strcmp(Transport(Connected, info, status), "unknown") == 0);
    status.state = Connected;
    info.state = 0;
    assert(!Current(Connected, info, status));
}

static void DescriptionPrivacy()
{
    ConnectionInfo info;
    std::strcpy(info.description, "#123 P2P SDR 192.0.2.1:27015 IP=[2001:db8::1]:27015");
    assert(std::strcmp(Description(info).data(), "#123 P2P SDR [redacted] [redacted]") == 0);
    std::strcpy(info.description, "P2P IP=198.51.100.1 ICE\ninjected \x7f");
    assert(std::strcmp(Description(info).data(), "P2P [redacted] [redacted] [redacted]") == 0);
    std::memset(info.description, 'x', sizeof(info.description));
    auto text = Description(info);
    assert(std::strlen(text.data()) == 128 && text.back() == 0);
    std::memset(info.description, '.', sizeof(info.description));
    assert(std::strcmp(Description(info).data(), "[redacted]") == 0);
    std::memset(info.description, 0, sizeof(info.description));
    assert(Description(info)[0] == 0);
}

int main()
{
    FlatApiLayout();
    RouteEvidence();
    DescriptionPrivacy();
    std::puts("Steam diagnostics: ABI, direct/SDR/unknown evidence and address redaction passed");
}
