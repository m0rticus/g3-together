// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/peer_timeout.h"
#include "../thirdparty/enet/protocol.c"
#include <assert.h>
#include <string.h>

static void check(enet_uint32 elapsed, unsigned attempts, int disconnected)
{
    ENetHost host;
    ENetPeer peer;
    ENetEvent event;
    ENetOutgoingCommand* command;
    memset(&host, 0, sizeof(host));
    memset(&peer, 0, sizeof(peer));
    memset(&event, 0, sizeof(event));
    host.mtu = 1200;
    host.connectedPeers = 1;
    host.serviceTime = 1 + elapsed;
    peer.host = &host;
    peer.state = ENET_PEER_STATE_CONNECTED;
    enet_list_clear(&peer.acknowledgements);
    enet_list_clear(&peer.sentReliableCommands);
    enet_list_clear(&peer.outgoingCommands);
    enet_list_clear(&peer.outgoingSendReliableCommands);
    enet_list_clear(&peer.dispatchedCommands);
    enet_peer_timeout(&peer, G3T_PEER_TIMEOUT_LIMIT, G3T_PEER_TIMEOUT_MINIMUM_MS, G3T_PEER_TIMEOUT_MAXIMUM_MS);
    command = (ENetOutgoingCommand*)enet_malloc(sizeof(*command));
    assert(command);
    memset(command, 0, sizeof(*command));
    command->sentTime = 1;
    command->roundTripTimeout = 100;
    command->sendAttempts = attempts;
    enet_list_insert(enet_list_end(&peer.sentReliableCommands), command);
    assert(enet_protocol_check_timeouts(&host, &peer, &event) == disconnected);
    if (disconnected) {
        assert(event.type == ENET_EVENT_TYPE_DISCONNECT);
        assert(peer.state == ENET_PEER_STATE_DISCONNECTED);
    } else {
        assert(peer.state == ENET_PEER_STATE_CONNECTED);
        enet_peer_reset(&peer);
    }
}

int main(void)
{
    check(60000, 6, 0);
    check(G3T_PEER_TIMEOUT_MINIMUM_MS - 1, 6, 0);
    check(G3T_PEER_TIMEOUT_MINIMUM_MS, 6, 1);
    check(G3T_PEER_TIMEOUT_MAXIMUM_MS - 1, 1, 0);
    check(G3T_PEER_TIMEOUT_MAXIMUM_MS, 1, 1);
    return 0;
}
