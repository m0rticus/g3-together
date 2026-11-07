// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/saving_policy.h"
#include "../src/npcwire_bones.h"
#include <cassert>
#include <limits>

int main()
{
    using namespace g3t::saving;
    static_assert(Protocol == 77, "host-managed save and guarded bone peers must use protocol 77");
    static_assert(g3t::npcwire::bones::Version == 74, "bone dictionary format version advances separately");
    const std::string world(32, 'a'), checkpoint(32, 'b'), previous(32, 'c');
    const Key alice{world, checkpoint, 123}, bob{world, checkpoint, 456};
    assert(Valid(alice) && !Valid({world, checkpoint, 0}));
    assert(!Id("../bad") && !Id(std::string(32, 'A')) && !Id(std::string(31, 'a')));
    assert(SameCharacter(alice, {world, previous, 123}));
    assert(!Exact(alice, {world, previous, 123}));
    assert(!SameCharacter(alice, bob));
    assert(!SameCharacter(alice, {previous, checkpoint, 123}));
    assert(Exact(alice, {world, checkpoint, 123}));
    assert(Prefix("g3t_" + checkpoint, "G3T_"));
    assert(Prefix("G3t_HoSt_save", "G3T_host_"));
    assert(!Prefix("G3", "G3T_") && !Prefix("myG3T_save", "G3T_"));
    assert(!NativeSaveRefusal(false, false, false, false)); // Combat/water/weapon/menu alone are not native save gates.
    assert(std::string(NativeSaveRefusal(true, false, false, false)) == "dialog");
    assert(std::string(NativeSaveRefusal(false, true, false, false)) == "control");
    assert(std::string(NativeSaveRefusal(false, false, true, false)) == "dead");
    assert(std::string(NativeSaveRefusal(false, false, false, true)) == "defeated");
    assert(std::string(NativeSaveRefusal(true, true, true, true)) == "dialog");
    int savedDowned = 9;
    assert(ParseRecordDowned("downed 0", 3, false, savedDowned) && savedDowned == 0);
    assert(ParseRecordDowned("downed 1", 4, false, savedDowned) && savedDowned == 1);
    for (auto line : {"downed", "downed 2", "downed -1", "downed 1 extra", "downed x", "downed 1x"}) {
        assert(!ParseRecordDowned(line, 3, false, savedDowned) && savedDowned == 1);
    }
    assert(!ParseRecordDowned("downed 0", 4, true, savedDowned) && savedDowned == 1);
    assert(!ParseRecordDowned("downed 0", 2, false, savedDowned) && savedDowned == 1);
    assert(!ParseRecordDowned("downed 0", 5, false, savedDowned) && savedDowned == 1);
    assert(!ParseRecordDowned(std::string("downed 0\0extra", 14), 4, false, savedDowned));

    const std::string identity = "identity " + world + " " + checkpoint + " 123";
    const std::string poseLine = "pose 12 -34 56 0 0 0 1";
    Key parsedKey;
    RecordPose parsedPose;
    assert(ParseRecordMetadata("G3THERO 4", identity, poseLine, &alice, parsedKey, parsedPose));
    assert(Exact(parsedKey, alice) && parsedPose.position[0] == 12 && parsedPose.position[1] == -34 &&
           parsedPose.position[2] == 56 && parsedPose.rotation[3] == 1);
    auto reject = [&](std::string const& header, std::string const& id, std::string const& pose, Key const* expected) {
        Key output = alice;
        RecordPose outputPose{{1, 2, 3}, {0, 0, 0, 1}};
        assert(!ParseRecordMetadata(header, id, pose, expected, output, outputPose));
        assert(Exact(output, alice) && outputPose.position[0] == 1 && outputPose.rotation[3] == 1);
    };
    reject("", identity, poseLine, &alice);
    reject("G3THERO 3", identity, poseLine, &alice);
    reject("G3THERO 4 extra", identity, poseLine, &alice);
    reject("G3THERO 4", "", poseLine, &alice);
    reject("G3THERO 4", identity, "", &alice);
    reject("G3THERO 4", "identity " + world, poseLine, &alice);
    reject("G3THERO 4", identity, "pose 12 -34 56 0 0 0", &alice);
    reject("G3THERO 4", identity, poseLine + " 1", &alice);
    reject("G3THERO 4", "identity " + std::string(32, 'A') + " " + checkpoint + " 123", poseLine, &alice);
    reject("G3THERO 4", "identity " + world + " " + std::string(31, 'b') + " 123", poseLine, nullptr);
    reject("G3THERO 4", "identity " + world + " " + checkpoint + " 0", poseLine, nullptr);
    reject("G3THERO 4", "identity " + world + " " + checkpoint + " -1", poseLine, nullptr);
    reject("G3THERO 4", "identity " + world + " " + checkpoint + " 18446744073709551616", poseLine, nullptr);
    reject("G3THERO 4", "identity " + world + " " + checkpoint + " 123x", poseLine, nullptr);
    reject("G3THERO 4", identity, poseLine, &bob);
    const Key otherWorld{previous, checkpoint, 123}, otherSave{world, previous, 123};
    reject("G3THERO 4", identity, poseLine, &otherWorld);
    reject("G3THERO 4", identity, poseLine, &otherSave);
    for (auto badPose : {"pose nan 0 0 0 0 0 1", "pose inf 0 0 0 0 0 1", "pose -inf 0 0 0 0 0 1",
                         "pose 10000001 0 0 0 0 0 1", "pose -10000001 0 0 0 0 0 1",
                         "pose 0 0 0 0 0 0 0", "pose 0 0 0 0 0 0 2", "pose 0 0 0 0 0 0 0.9", "pose 0 0 0 0 0 0 1.006",
                         "pose 0 0 0 nan 0 0 1", "pose 0 0 0 0 0 0 1e999",
                         "pose 0x 0 0 0 0 0 1"})
        reject("G3THERO 4", identity, badPose, &alice);
    reject("G3THERO 4", identity, poseLine + std::string("\0hidden", 7), &alice);
    assert(ParseRecordMetadata("G3THERO 4", identity, "pose 10000000 -10000000 0 0 0 0 -1", nullptr, parsedKey, parsedPose));
    assert(ParseRecordMetadata("G3THERO 4", identity, "pose 0 0 0 0 0 0 0.995", &alice, parsedKey, parsedPose));
    assert(ParseRecordMetadata("G3THERO 4", identity, "pose 0 0 0 0 0 0 1.004", &alice, parsedKey, parsedPose));
    assert(ParseRecordMetadata("G3THERO 4", "identity " + world + " " + checkpoint + " 18446744073709551615",
                               poseLine, nullptr, parsedKey, parsedPose));
    RecordPose invalidPose{{0, 0, 0}, {0, 0, 0, 1}};
    invalidPose.position[0] = std::numeric_limits<float>::quiet_NaN();
    assert(!ValidRecordPose(invalidPose));
    invalidPose.position[0] = 0;
    invalidPose.rotation[0] = std::numeric_limits<float>::infinity();
    assert(!ValidRecordPose(invalidPose));

    assert(StrictLegacyReady(true, true, true, true, true));
    assert(!StrictLegacyReady(false, true, true, true, true));
    assert(!StrictLegacyReady(true, false, true, true, true));
    assert(!StrictLegacyReady(true, true, false, true, true));
    assert(!StrictLegacyReady(true, true, true, false, true));
    assert(!StrictLegacyReady(true, true, true, true, false));
    RecordAttribute attribute;
    assert(ParseRecordAttribute("attr STR 100 20", attribute));
    assert(attribute.tag == "STR" && attribute.base == 100 && attribute.modifier == 20 &&
           attribute.baseMaximum == 0 && attribute.maximumModifier == 0);
    assert(ParseRecordAttribute("attr HP 125 0 200 30", attribute));
    assert(attribute.tag == "HP" && attribute.base == 125 && attribute.modifier == 0 &&
           attribute.baseMaximum == 200 && attribute.maximumModifier == 30);
    assert(ParseRecordAttribute("attr MP 30 0 200 -25", attribute));
    assert(attribute.baseMaximum == 200 && attribute.maximumModifier == -25);
    assert(ParseRecordAttribute("attr SP 0 0 0 0", attribute));
    const RecordAttribute unchanged = attribute;
    for (auto line : {"attr HP 125 0", "attr STR 100 20 200 30", "attr HP 125 0 200",
                      "attr STR 100 20 extra", "attr UNKNOWN 1 2", "attr STR nan 0",
                      "attr STR 1 inf", "attr HP 1 0 1 9999999", "attr HP 1 0 1000000 1",
                      "attr STR 1000000 1", "attr STR -1000000 -1",
                      "attr STR 2147483648 0", "attr STR 1 0 0 0"}) {
        assert(!ParseRecordAttribute(line, attribute));
        assert(attribute.tag == unchanged.tag && attribute.base == unchanged.base &&
               attribute.baseMaximum == unchanged.baseMaximum);
    }
    assert(LegacyRecordAttribute("STR", 100, 120, attribute));
    assert(attribute.base == 100 && attribute.modifier == 20);
    assert(LegacyRecordAttribute("HP", 80, 230, attribute));
    assert(attribute.base == 80 && attribute.modifier == 0 && attribute.baseMaximum == 230 && attribute.maximumModifier == 0);
    assert(!LegacyRecordAttribute("STR", -1000000, 1000000, attribute));
    assert(!LegacyRecordAttribute("UNKNOWN", 0, 0, attribute));
    RecordAttribute outOfRange{"HP", 1, 0, 1000000, 1};
    assert(!ValidRecordAttribute(outOfRange));

    assert(SafeName("G3T_Sandbox") && SafeName("G3_World_01_0002"));
    assert(SafeName("G3_World_01_0002") && SafeName("My test-save 2"));
    assert(SafeName(std::string(48, 'x').c_str()) && !SafeName(std::string(49, 'x').c_str()));
    for (auto name : {"", "..\\escape", "../escape", "C:stream", "slot.txt", "name ", "CON", "nul", "COM1", "lpt9"})
        assert(!SafeName(name));
    assert(!SafeName(nullptr));


}
