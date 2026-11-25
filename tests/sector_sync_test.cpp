// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/sector_sync_policy.h"

#include <cassert>
#include <cstring>
#include <string>
#include <vector>

using namespace g3t::sector_sync;

static void TestWireValidation()
{
    assert(IsHandshakeWire(WorldStateKind, FlagRequest, ""));
    assert(!IsHandshakeWire(WorldStateKind, FlagRequest, "G3_Myrtana_01_Ardea_NPC_02"));
    assert(IsHandshakeWire(WorldStateKind, FlagBaseline, ""));
    assert(IsHandshakeWire(WorldStateKind, FlagBaseline, "G3_Myrtana_01_Ardea_NPC_02"));
    assert(IsHandshakeWire(WorldStateKind, FlagBaseline | FlagEnd, ""));
    assert(!IsHandshakeWire(WorldStateKind, FlagBaseline | FlagEnd, "extra"));
    assert(IsHandshakeWire(WorldStateKind, FlagBaseline | FlagCatchup, "G3_Myrtana_01_Ardea_NPC_02"));
    assert(!IsHandshakeWire(WorldStateKind, FlagBaseline | FlagCatchup, ""));
    assert(!IsHandshakeWire(WorldStateKind, 0, "G3_Myrtana_01_Ardea_NPC_02"));
    assert(!IsHandshakeWire(1, FlagRequest, ""));

    assert(ValidSendWorldStateTo(false, WorldStateKind, FlagRequest, ""));
    assert(!ValidSendWorldStateTo(true, WorldStateKind, FlagRequest, ""));
    assert(ValidSendWorldStateTo(true, WorldStateKind, FlagBaseline, ""));
    assert(ValidSendWorldStateTo(true, WorldStateKind, FlagBaseline, "G3_Myrtana_01_Ardea_NPC_02"));
    assert(ValidSendWorldStateTo(true, WorldStateKind, FlagBaseline | FlagEnd, ""));
    assert(ValidSendWorldStateTo(true, WorldStateKind, FlagBaseline | FlagCatchup, "G3_Myrtana_01_Ardea_NPC_02"));
    assert(!ValidSendWorldStateTo(false, WorldStateKind, FlagBaseline, ""));

    uint8_t guid[16]{};
    PackCount(guid, 0x12345678u);
    assert(UnpackCount(guid) == 0x12345678u);
}

static void TestHostAndClientAdmissionReconciliation()
{
    HostState host;
    StateMap initial{
        {"G3_Myrtana_01_Ardea_NPC_01", true},
        {"G3_Myrtana_01_Ardea_NPC_02", false},
        {"G3_Myrtana_01_Montera_NPC_01", true},
    };
    host.Seed(initial);
    assert(host.seeded);

    // Console / script toggle flips Ardea sectors before client joins.
    StateMap toggled = initial;
    toggled["G3_Myrtana_01_Ardea_NPC_01"] = false;
    toggled["G3_Myrtana_01_Ardea_NPC_02"] = true;
    const auto deltas = host.Reconcile(toggled);
    assert(deltas.size() == 2);
    assert(deltas[0].first == "G3_Myrtana_01_Ardea_NPC_01" && deltas[0].second == false);
    assert(deltas[1].first == "G3_Myrtana_01_Ardea_NPC_02" && deltas[1].second == true);
    assert(host.Reconcile(toggled).empty());

    // Client requests baseline while host continues playing.
    const StateMap* snap = host.BeginReplay(2, 10, toggled);
    assert(snap && snap->size() == 3);
    host.MarkReplaySent(2, 10);

    ClientState client;
    client.PrepareAdmission();
    assert(client.admissionStarted && !client.complete);
    // Reject entries before Begin.
    assert(!client.OnBaselineMessage(FlagBaseline, "G3_Myrtana_01_Ardea_NPC_02", true, 0));
    assert(client.OnBaselineMessage(FlagBaseline, "", false, 0));
    // Reject wrong count on End.
    assert(client.OnBaselineMessage(FlagBaseline, "G3_Myrtana_01_Ardea_NPC_01", false, 0));
    assert(!client.OnBaselineMessage(FlagBaseline | FlagEnd, "", false, 3));
    assert(!client.complete && !client.begun);

    // Valid full replay + concurrent live delta during admission download.
    assert(client.OnBaselineMessage(FlagBaseline, "", false, 0));
    for (auto const& [name, enabled] : *snap)
        assert(client.OnBaselineMessage(FlagBaseline, name.c_str(), enabled, 0));
    client.OnLiveDelta("G3_Myrtana_01_Montera_NPC_01", false);
    assert(client.OnBaselineMessage(FlagBaseline | FlagEnd, "", false, static_cast<uint32_t>(snap->size())));
    assert(client.complete);

    StateMap committed;
    assert(client.TakeCommitMap(committed));
    assert(client.admissionApplied);
    assert(committed.at("G3_Myrtana_01_Ardea_NPC_01") == false);
    assert(committed.at("G3_Myrtana_01_Ardea_NPC_02") == true);
    assert(committed.at("G3_Myrtana_01_Montera_NPC_01") == false);

    // Host FinishAdmission sends any changes since BeginReplay.
    StateMap afterCutover = toggled;
    afterCutover["G3_Myrtana_01_Montera_NPC_01"] = false;
    DeltaList catchup;
    assert(host.FinishReplay(2, 10, afterCutover, catchup));
    assert(catchup.size() == 1 && catchup[0].first == "G3_Myrtana_01_Montera_NPC_01" && catchup[0].second == false);

    // Same-generation template rejoin resets finished replay cleanly.
    const StateMap* rejoinSnap = host.BeginReplay(2, 10, afterCutover);
    assert(rejoinSnap && rejoinSnap->at("G3_Myrtana_01_Montera_NPC_01") == false);
}

static void TestExtensionCodec()
{
    StateMap decoded;
    bool present = true;
    assert(DecodeExtension(0, {}, decoded, present) && !present && decoded.empty());

    StateMap input{
        {"G3_Myrtana_01_Ardea_NPC_01", false},
        {"G3_Myrtana_01_Ardea_NPC_02", true},
        {"G3_Varant_01_MoraSul_Temple_NSCs_02", true},
    };
    std::vector<uint8_t> encoded;
    assert(EncodeExtension(input, encoded));
    assert(DecodeExtension(ExtensionVersion, encoded, decoded, present) && present && decoded == input);

    // Reject corrupt magic, wrong version, truncated, trailing byte, invalid bool.
    assert(!DecodeExtension(ExtensionVersion + 1, encoded, decoded, present));
    auto badMagic = encoded;
    badMagic[0] ^= 0xff;
    assert(!DecodeExtension(ExtensionVersion, badMagic, decoded, present));
    auto truncated = encoded;
    truncated.pop_back();
    assert(!DecodeExtension(ExtensionVersion, truncated, decoded, present));
    auto trailing = encoded;
    trailing.push_back(0);
    assert(!DecodeExtension(ExtensionVersion, trailing, decoded, present));
    auto badBool = encoded;
    badBool[12] = 2;
    assert(!DecodeExtension(ExtensionVersion, badBool, decoded, present));
}

int main()
{
    TestWireValidation();
    TestHostAndClientAdmissionReconciliation();
    TestExtensionCodec();
    return 0;
}
