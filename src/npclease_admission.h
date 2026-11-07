// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace g3t::npclease {
enum class NativeClass { Unsupported, Creature, Ordinary };

struct Admission {
    NativeClass type = NativeClass::Unsupported;
    bool complete = false, mortal = false, independent = false, portableCreature = false;
    bool storyCatalog = false, storyReferenced = true;

    char const* Refusal() const
    {
        if (!complete) return "incomplete native NPC/template";
        if (!mortal) return "immortal/story actor";
        if (!independent) return "trader/teacher/party/summon/owned actor";
        if (type == NativeClass::Unsupported) return "unsupported native species";
        if (type == NativeClass::Creature)
            return portableCreature ? nullptr : "named creature or unportable creature equipment";
        if (!storyCatalog) return "authoritative story catalog unavailable";
        if (storyReferenced) return "authored quest/dialogue reference; visual-only";
        return nullptr;
    }
};

inline bool HostUnknown(bool nativeInstance, bool npcRecord, bool leaseBaseline)
{
    return !nativeInstance && !npcRecord && !leaseBaseline;
}

inline bool MayDelegate(NativeClass type, bool hostUnknown, bool acceptedDiscovery)
{
    return type == NativeClass::Creature ||
        (type == NativeClass::Ordinary && (hostUnknown || acceptedDiscovery));
}
}
