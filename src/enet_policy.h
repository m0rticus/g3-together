// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <enet/enet.h>

namespace g3t::net {
constexpr enet_uint32 PacketFlags(bool reliable)
{
    // Let ENet use the negotiated MTU/header budget; a payload below the MTU can still need fragments.
    return reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
}
}
