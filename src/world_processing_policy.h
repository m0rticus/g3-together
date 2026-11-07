// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstring>

namespace g3t::world {
// CP 1.75 FourceFullROIUpdate clears membership before setting this dirty byte.
// Request only its recollection step; Process preserves still-eligible members.
inline bool QueueProcessingRebuild(unsigned char* admin, unsigned char const* refresh)
{
    constexpr unsigned char signature[] = {
        0x56, 0x8b, 0xf1, 0xe8, 0xf8, 0xfb, 0xff, 0xff,
        0xc6, 0x86, 0xfd, 0x00, 0x00, 0x00, 0x01, 0x5e, 0xc3
    };
    if (!admin || !refresh || std::memcmp(refresh, signature, sizeof(signature))) return false;
    admin[0xfd] = 1;
    return true;
}
}
