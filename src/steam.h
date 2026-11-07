// SPDX-License-Identifier: GPL-3.0-or-later
// Steam transport (owner 27.10.: co-op over Steam, no IP address, no port forwarding, no Hamachi). ENet stays: its UDP
// datagrams travel between the players over Steam (ISteamNetworkingMessages, relayed by Valve where no direct path
// exists). Uses Gothic 3's AppID 39500; both players must own the Steam game. Kept free of SDK and winsock headers,
// like net.h.
// session_id filters lobby metadata before joining; every relay message also carries that ID and the Steam lobby number.
#pragma once

#include <cstdint>

namespace g3t::steam {
struct TrafficCounters
{
    uint32_t envelopeBytes = 16;
    uint64_t txDatagrams = 0;
    uint64_t txBytes = 0;
    uint64_t txEnvelopeBytes = 0;
    uint64_t rxDatagrams = 0;
    uint64_t rxBytes = 0;
    uint64_t rxEnvelopeBytes = 0;
    uint64_t txDataDatagrams = 0;
    uint64_t txDataBytes = 0;
    uint64_t txEnetProtocolBytes = 0;
    uint64_t txEnetAckBytes = 0;
    uint64_t txEnetParseFailures = 0;
    uint64_t txEnetUnparsedDatagrams = 0;
};

// Loads required steam_api.dll, starts the Steam API as Gothic 3 and, for a host, opens a friends-only lobby. enetPort: the host's
// ENet port (each friend's datagrams go there over 127.0.0.1). False if Steam cannot be used; Error() says why.
bool Start(bool host, unsigned short enetPort);
// Client: the 127.0.0.1 port ENet connects to; the relay passes it on to the host found over Steam. 0 before Start.
unsigned short RelayPort();
bool IsPeerRelay(unsigned short port);  // host: this loopback sender belongs to a linked Steam lobby member
uint64_t PeerId(unsigned short port);  // account authenticated by the relay; 0 if not linked
// Wake the independent transport worker after ENet flushes. Does not run engine or ENet handlers.
void Pump();
// Join the transport worker, forward ENet's goodbye, then leave/close Steam. Called on the engine thread.
void Stop();

bool Ready();               // the Steam API is running
uint64_t LobbyId();
int Ping();                 // maximum QPC control RTT, rounded ms; -1 before a sample or after 5 s without replies
char const* PlayerName();   // Steam persona truncated at a complete UTF-8 boundary for the protocol's 23-byte name
uint64_t PlayerId();       // authenticated local account, stable across names and reconnects; 0 before Steam starts
char const* Error();        // why Steam is not used, "" while it is (or before Start)
// Client: the Steam name of the host whose lobby it is in ("" while it looks for one) and whether that host answered.
char const* HostName();
bool HostLinked();
// Host: friends whose relay is linked to this game.
int LinkedCount();
TrafficCounters Traffic(); // successful Steam API submissions and accepted relay datagrams, copied by the existing published mutex
// QA (steamstatus): one line with role, lobby, peers, relay counters.
void Status(void (*log)(char const*));
}
