// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/npcidentity_policy.h"

#include <cassert>

using namespace g3t::npcidentity;

int main()
{
    Candidate keeper{1, {}, {}, "Orc_01", false};
    keeper.guid[0] = 1;
    keeper.templateId[0] = 2;
    Candidate extra = keeper;
    extra.instance = 2;
    assert(Quarantine(extra, keeper, keeper.instance));
    assert(Quarantine(extra, keeper, 0)); // Native-only region and a released lease still have one GUID resolver.
    assert(!Quarantine(keeper, keeper, 1));
    assert(!Quarantine(extra, keeper, extra.instance)); // Do not hide anything while mirror rebinding is pending.
    for (int byte = 0; byte < 16; ++byte) {
        auto different = extra;
        different.guid[byte] ^= 0x80;
        assert(!Quarantine(different, keeper, 1));
        different = extra;
        different.templateId[byte] ^= 0x80;
        assert(!Quarantine(different, keeper, 1));
    }
    auto protectedExtra = extra;
    protectedExtra.protectedRole = true;
    assert(!Quarantine(protectedExtra, keeper, 1));
    auto protectedKeeper = keeper;
    protectedKeeper.protectedRole = true;
    assert(!Quarantine(extra, protectedKeeper, 1));
    auto renamed = extra;
    renamed.name = "Orc_02";
    assert(!Quarantine(renamed, keeper, 1));
    auto unnamed = keeper;
    unnamed.name.clear();
    assert(!Quarantine(extra, unnamed, 1));
    auto invalid = keeper;
    invalid.guid.fill(0);
    auto invalidExtra = invalid;
    invalidExtra.instance = 2;
    assert(!Quarantine(invalidExtra, invalid, 1));
    invalid.templateId.fill(0);
    invalid.guid = keeper.guid;
    invalidExtra = invalid;
    invalidExtra.instance = 2;
    assert(!Quarantine(invalidExtra, invalid, 1));
    std::unordered_set<uintptr_t> seen;
    assert(!AddPointer(seen, 0));
    assert(AddPointer(seen, 1));
    assert(!AddPointer(seen, 1)); // Same pointer in ROI and dynamic inventory is not another model.
    assert(AddPointer(seen, 2));
    assert(seen.size() == 2);
    seen.clear();
    assert(AddPointer(seen, 1)); // A new census/world is independent of the old one.
    assert(Quarantine(extra, keeper, 1)); // Repeated checks must still fence a re-enabled extra.
    assert(Quarantine(keeper, extra, 2)); // GUID resolver changes preserve its new keeper.
}
