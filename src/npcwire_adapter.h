// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "net.h"
#include "npcwire_control.h"

namespace g3t::npcwire {

bool FromNpcRecord(net::NpcRecord const& record, std::uint32_t anchor, std::uint16_t baseline,
                   bool fullRotation, Control& output);
bool ToNpcRecord(Control const& control, net::NpcRecord& output);

}
