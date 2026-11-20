// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/reputation_policy.h"
#include "../src/story_policy.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace rep = g3t::reputation;
namespace sp = g3t::story::policy;

namespace {

struct SimWorld {
    std::vector<int32_t> political = std::vector<int32_t>(rep::MinNativePoliticalArraySize, 0);
    std::map<std::string, int32_t> enclaves{
        {"Ardea", 0}, {"Reddock", 0}, {"CapeDun", 0}, {"Montera", 0},
        {"Gotha", 0}, {"Bakaresh", 0}, {"MoraSul", 0}, {"Ishtar", 0},
    };
    std::map<std::string, int32_t> enclaveStatus{
        {"Reddock", 0}, {"CapeDun", 2}, {"Montera", 2}, {"Bakaresh", 0},
    };
    std::map<std::string, int32_t> crimes{
        {"Reddock", 0}, {"CapeDun", 0}, {"Montera", 0}, {"Bakaresh", 0},
    };
    std::map<std::string, uint32_t> quests{
        {"Reddock_Liberate", 1},
        {"Bakaresh_Arena", 1},
    };

    int32_t GetPol(size_t idx) const { return idx < political.size() ? political[idx] : 0; }

    void SetPol(size_t idx, int32_t val)
    {
        const size_t needed = rep::RequiredPoliticalArrayCount(political.size(), idx, val);
        if (political.size() < needed) political.resize(needed, 0);
        if (idx < political.size()) political[idx] = val;
        assert(political.size() >= rep::MinNativePoliticalArraySize);
    }

    std::map<std::string, uint32_t> ReputationSnapshot() const
    {
        std::map<std::string, uint32_t> out;
        for (size_t i = 0; i < rep::PoliticalAlignmentCount; ++i)
            out[rep::PoliticalKey(i)] = rep::EncodeFame(GetPol(i));
        for (auto const& [name, fame] : enclaves) {
            const std::string key = rep::EnclaveKey(name);
            out[key] = rep::EncodeFame(fame);
        }
        return out;
    }

    void ApplyReputationEntry(std::string const& key, uint32_t encoded)
    {
        bool isPol = false;
        size_t idx = 0;
        std::string encName;
        assert(rep::ParseKey(key, isPol, idx, encName));
        const int32_t fame = rep::DecodeFame(encoded);
        if (isPol) SetPol(idx, fame);
        else enclaves[encName] = fame;
    }

    // Mirrors Game.dll gCQuest_PS::SetStatus (0x10157840): transitioning into Succeeded(3) awards quest fame.
    void NativeSetQuestStatus(std::string const& quest, uint32_t nextStatus, int polIdx, int32_t polGain,
                              std::string const& enclave, int32_t encGain)
    {
        const uint32_t prev = quests[quest];
        if (prev == nextStatus) return;
        quests[quest] = nextStatus;
        if (nextStatus == 3) {
            if (polIdx >= 0) SetPol(static_cast<size_t>(polIdx), rep::ApplyClampedDelta(GetPol(static_cast<size_t>(polIdx)), polGain));
            if (!enclave.empty()) enclaves[enclave] = rep::ApplyClampedDelta(enclaves[enclave], encGain);
        }
    }

    // Mirrors story.cpp QuestApply on Client: preserves political & enclave fame across native SetStatus.
    void ClientQuestApply(std::string const& quest, uint32_t nextStatus, int polIdx, int32_t polGain,
                          std::string const& enclave, int32_t encGain)
    {
        const auto savedPol = political;
        const auto savedEnc = enclaves;
        NativeSetQuestStatus(quest, nextStatus, polIdx, polGain, enclave, encGain);
        political = savedPol;
        enclaves = savedEnc;
    }
};

void AssertReputationMatches(SimWorld const& host, SimWorld const& client)
{
    for (size_t i = 0; i < rep::PoliticalAlignmentCount; ++i)
        assert(client.GetPol(i) == host.GetPol(i));
    assert(client.enclaves == host.enclaves);
    assert(client.political.size() >= rep::MinNativePoliticalArraySize);
}

void RunPreAdmissionBaseline(SimWorld const& host, SimWorld& client)
{
    sp::Baseline baseline;
    baseline.Begin();
    for (auto const& [key, encoded] : host.ReputationSnapshot()) {
        std::string wireName;
        assert(rep::PackState(key, encoded, 63, wireName));
        std::string unpackedKey;
        uint32_t unpackedVal = 0;
        assert(rep::UnpackState(wireName, unpackedKey, unpackedVal));
        assert(baseline.Add(unpackedKey, unpackedVal));
    }
    assert(baseline.Finish() && baseline.complete);

    // CommitBaselines: omitted keys reset to 0u -> DecodeFame(0u) == 0, then every baseline value applies.
    for (size_t i = 0; i < rep::PoliticalAlignmentCount; ++i) {
        const std::string k = rep::PoliticalKey(i);
        if (!baseline.values.count(k)) client.ApplyReputationEntry(k, 0);
    }
    for (auto const& [name, _] : client.enclaves) {
        const std::string k = rep::EnclaveKey(name);
        if (!baseline.values.count(k)) client.ApplyReputationEntry(k, 0);
    }
    for (auto const& [k, v] : baseline.values) client.ApplyReputationEntry(k, v);
}

void TestWireCodecAndValidation()
{
    assert(rep::EncodeFame(0) == 0u && rep::DecodeFame(0u) == 0);
    const int32_t samples[] = {
        0, 1, -1, 2, -2, 50, -50, 100, -100, 1000, -1000,
        std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min(),
    };
    for (int32_t s : samples) assert(rep::DecodeFame(rep::EncodeFame(s)) == s);

    for (size_t i = 0; i < rep::PoliticalAlignmentCount; ++i) {
        const std::string key = rep::PoliticalKey(i);
        std::string wire, outKey;
        uint32_t outVal = 999;
        assert(rep::PackState(key, rep::EncodeFame(static_cast<int32_t>(i * 7 - 21)), 63, wire));
        assert(wire.size() <= 63);
        assert(rep::UnpackState(wire, outKey, outVal));
        assert(outKey == key);
        assert(rep::DecodeFame(outVal) == static_cast<int32_t>(i * 7 - 21));
    }

    for (char const* town : {"Ardea", "Reddock", "Montera", "Bakaresh", "MoraSul", "Ishtar", "Faring"}) {
        const std::string key = rep::EnclaveKey(town);
        std::string wire, outKey;
        uint32_t outVal = 0;
        assert(rep::PackState(key, rep::EncodeFame(75), 63, wire));
        assert(rep::UnpackState(wire, outKey, outVal) && outKey == key && rep::DecodeFame(outVal) == 75);
    }

    bool isPol = true;
    size_t idx = 99;
    std::string enc;
    for (char const* badKey : {
             "", "@pol/", "@pol/-1", "@pol/15", "@pol/01", "@pol/abc",
             "@enc/", "@other/1", "Reddock", "@enc/Bad\x1f" "Name",
         }) {
        assert(!rep::ParseKey(badKey, isPol, idx, enc));
    }
    std::string tooLong(64, 'A');
    std::string wireTmp;
    assert(!rep::PackState(rep::EnclaveKey(tooLong), rep::EncodeFame(10), 63, wireTmp));
    assert(!rep::PackState(rep::EnclaveKey(""), rep::EncodeFame(10), 63, wireTmp));

    std::string outKey = "keep";
    uint32_t outVal = 42;
    for (char const* badWire : {
             "", "@pol/3", "@pol/3\x1f", "@pol/3\x1f" "4294967296", "@pol/15\x1f" "2",
             "@enc/\x1f" "2", "@enc/Reddock\x1f" "-1", "@enc/Reddock\x1f" "00",
         }) {
        assert(!rep::UnpackState(badWire, outKey, outVal));
        assert(outKey == "keep" && outVal == 42);
    }
}

void TestPoliticalArraySafety()
{
    assert(rep::RequiredPoliticalArrayCount(0, 3, 25) == 10);
    assert(rep::RequiredPoliticalArrayCount(10, 3, 0) == 10);
    assert(rep::RequiredPoliticalArrayCount(10, 13, 0) == 10);   // zero extended alignment keeps native 10-slot array untouched
    assert(rep::RequiredPoliticalArrayCount(10, 13, 15) == 14);  // non-zero Bakaresh (13) expands to 14 with zero-filled gap
    assert(rep::RequiredPoliticalArrayCount(14, 14, 5) == 15);   // Gonzalez (14) expands up to PoliticalAlignmentCount (15)
}

void TestJoinReconnectAndHostSaveLoad()
{
    SimWorld host, client;
    // Host has progress across Rebels(3), Hashishin(5), Nomads(6), Nordmar(2), Orcs(1), Rangers/Mid(4), Bakaresh(13).
    host.SetPol(1, 15);   // Orc / Mercenaries
    host.SetPol(2, 20);   // Nordmar
    host.SetPol(3, 45);   // Rebels
    host.SetPol(4, 10);   // Rangers / Midland
    host.SetPol(5, 60);   // Hashishin
    host.SetPol(6, 30);   // Nomads
    host.SetPol(13, 25);  // Bakaresh camp alignment
    host.enclaves["Reddock"] = 85;
    host.enclaves["Montera"] = 55;
    host.enclaves["Bakaresh"] = 70;

    // Client connects with mismatched/stale values (including camps & enclaves that are 0 on Host).
    client.SetPol(1, 90);
    client.SetPol(3, 5);
    client.SetPol(9, 50);   // Pirate = 50 locally, 0 on host
    client.SetPol(14, 40);  // Gonzalez = 40 locally, 0 on host
    client.enclaves["Ardea"] = 100;
    client.enclaves["Reddock"] = 10;

    RunPreAdmissionBaseline(host, client);
    AssertReputationMatches(host, client);
    assert(client.GetPol(9) == 0 && client.GetPol(14) == 0 && client.enclaves["Ardea"] == 0);

    // Reconnect after host gained more reputation while client was offline.
    host.SetPol(5, 80);
    host.enclaves["Ishtar"] = 65;
    RunPreAdmissionBaseline(host, client);
    AssertReputationMatches(host, client);

    // Host quick-loads an earlier save where Hashishin(5)=0, Bakaresh(13)=0, Ishtar=0.
    host.SetPol(5, 0);
    host.SetPol(13, 0);
    host.enclaves["Ishtar"] = 0;
    host.enclaves["Bakaresh"] = 20;
    RunPreAdmissionBaseline(host, client);
    AssertReputationMatches(host, client);
    assert(client.GetPol(5) == 0 && client.GetPol(13) == 0 && client.enclaves["Ishtar"] == 0);
}

void TestQuestCompletionBothPlayersAndPacketOrders()
{
    // 1. Host completes quest; StateQuests arrives BEFORE StateReputation on client.
    {
        SimWorld host, client;
        host.SetPol(3, 20);
        host.enclaves["Reddock"] = 50;
        RunPreAdmissionBaseline(host, client);
        auto hostKnown = host.ReputationSnapshot();

        host.NativeSetQuestStatus("Reddock_Liberate", 3, 3, 15, "Reddock", 25);
        assert(host.GetPol(3) == 35 && host.enclaves["Reddock"] == 75);
        const auto repDelta = sp::Changes(hostKnown, host.ReputationSnapshot());

        // Client receives StateQuests first: QuestApply must NOT mutate client reputation locally.
        client.ClientQuestApply("Reddock_Liberate", 3, 3, 15, "Reddock", 25);
        assert(client.GetPol(3) == 20 && client.enclaves["Reddock"] == 50);

        // Client then receives StateReputation delta.
        for (auto const& [k, v] : repDelta) client.ApplyReputationEntry(k, v);
        AssertReputationMatches(host, client);
    }

    // 2. Host completes quest; StateReputation arrives BEFORE StateQuests on client.
    {
        SimWorld host, client;
        host.SetPol(5, 30);
        host.enclaves["Bakaresh"] = 40;
        RunPreAdmissionBaseline(host, client);
        auto hostKnown = host.ReputationSnapshot();

        host.NativeSetQuestStatus("Bakaresh_Arena", 3, 5, 10, "Bakaresh", 20);
        const auto repDelta = sp::Changes(hostKnown, host.ReputationSnapshot());

        for (auto const& [k, v] : repDelta) client.ApplyReputationEntry(k, v);
        assert(client.GetPol(5) == 40 && client.enclaves["Bakaresh"] == 60);

        // StateQuests arrives second: ClientQuestApply preserves the already-applied 40 / 60 without double-counting!
        client.ClientQuestApply("Bakaresh_Arena", 3, 5, 10, "Bakaresh", 20);
        AssertReputationMatches(host, client);
    }

    // 3. Client completes quest locally -> forwards StateQuests to Host -> Host applies & broadcasts StateReputation.
    {
        SimWorld host, client;
        host.SetPol(3, 10);
        host.enclaves["Reddock"] = 30;
        RunPreAdmissionBaseline(host, client);
        auto hostKnown = host.ReputationSnapshot();

        // Client's own gameplay runs SucceedQuest locally (+15 / +25) and sends StateQuests to Host.
        client.NativeSetQuestStatus("Reddock_Liberate", 3, 3, 15, "Reddock", 25);
        assert(client.GetPol(3) == 25 && client.enclaves["Reddock"] == 55);

        // Host receives StateQuests, runs SucceedQuest (+15 / +25 on Host's 10 / 30 -> 25 / 55), and flushes StateReputation.
        host.NativeSetQuestStatus("Reddock_Liberate", 3, 3, 15, "Reddock", 25);
        const auto repDelta = sp::Changes(hostKnown, host.ReputationSnapshot());
        client.ClientQuestApply("Reddock_Liberate", 3, 3, 15, "Reddock", 25);
        for (auto const& [k, v] : repDelta) client.ApplyReputationEntry(k, v);
        AssertReputationMatches(host, client);
        assert(client.GetPol(3) == 25 && client.enclaves["Reddock"] == 55);
    }
}

void TestDialogReputationAndLiberationAndPersonalMode()
{
    rep::DialogRepMutation mut{};
    // Script_Game.dll IS_Running_ProcessInfo (0x100b7f10): ID1=camp token, ID2=amount, TextID="-" for subtract (else add), Entity2=enclave.
    assert(rep::ParseDialogReputationCommand("ModReputPol", "Ass", "10", "+", nullptr, mut));
    assert(mut.isPolitical && mut.alignment == 5 && mut.delta == 10);

    assert(rep::ParseDialogReputationCommand("ModReputPol", "Reb", "25", "-", nullptr, mut));
    assert(mut.isPolitical && mut.alignment == 3 && mut.delta == -25);

    assert(rep::ParseDialogReputationCommand("ModReputPol", "Nom", "5", "", nullptr, mut));
    assert(mut.isPolitical && mut.alignment == 6 && mut.delta == 5);

    assert(rep::ParseDialogReputationCommand("ModReputPol", "Pir", "15", "+", nullptr, mut));
    assert(mut.isPolitical && mut.alignment == 9 && mut.delta == 15);

    assert(rep::ParseDialogReputationCommand("ModReputEnc", "", "15", "+", "Montera", mut));
    assert(!mut.isPolitical && mut.enclave == "Montera" && mut.delta == 15);

    assert(rep::ParseDialogReputationCommand("ModReputEnc", "", "10", "-", "Bakaresh", mut));
    assert(!mut.isPolitical && mut.enclave == "Bakaresh" && mut.delta == -10);

    // Invalid commands / zero / missing target rejected cleanly.
    assert(!rep::ParseDialogReputationCommand("GiveXP", "Ass", "10", "+", nullptr, mut));
    assert(!rep::ParseDialogReputationCommand("ModReputPol", "UnknownCamp", "10", "+", nullptr, mut));
    assert(!rep::ParseDialogReputationCommand("ModReputPol", "Ass", "0", "+", nullptr, mut));
    assert(!rep::ParseDialogReputationCommand("ModReputPol", "Ass", "abc", "+", nullptr, mut));
    assert(!rep::ParseDialogReputationCommand("ModReputEnc", "", "10", "+", nullptr, mut));
    assert(!rep::ParseDialogReputationCommand("ModReputEnc", "", "10", "+", "", mut));

    // Clamping [0, 100] matches Script_Game.dll ModReputPol (0x100b6880) / ModReputEnc (0x100b69c0).
    assert(rep::ApplyClampedDelta(95, 20) == 100);
    assert(rep::ApplyClampedDelta(5, -20) == 0);
    assert(rep::ApplyClampedDelta(-10, 5) == 0);
    assert(rep::ApplyClampedDelta(40, 15) == 55);

    // Client runs ModReputPol("Ass", "+", "15") & ModReputEnc("Bakaresh", "-", "50") while crime mode is Personal:
    // personal crimes stay separate, while both camp and enclave reputations match Host!
    SimWorld host, client;
    host.SetPol(5, 90);
    host.enclaves["Bakaresh"] = 30;
    RunPreAdmissionBaseline(host, client);
    client.crimes["Bakaresh"] = 3;  // personal theft count on client

    // Client forwards EnclaveRepDelta -> Host clamps & broadcasts StateReputation + EnclaveState.
    host.SetPol(5, rep::ApplyClampedDelta(host.GetPol(5), 15));
    host.enclaves["Bakaresh"] = rep::ApplyClampedDelta(host.enclaves["Bakaresh"], -50);
    for (auto const& [k, v] : host.ReputationSnapshot()) client.ApplyReputationEntry(k, v);
    AssertReputationMatches(host, client);
    assert(host.GetPol(5) == 100 && client.GetPol(5) == 100);
    assert(host.enclaves["Bakaresh"] == 0 && client.enclaves["Bakaresh"] == 0);
    assert(client.crimes["Bakaresh"] == 3 && host.crimes["Bakaresh"] == 0);

    // Town liberation (ChangeEnclaveStatus 0x100602d0): Attack(2)/Flee(1) -> Routine(0) sets enclave fame = 100.
    assert(rep::ShouldAwardLiberationFame(2, 0));
    assert(rep::ShouldAwardLiberationFame(1, 0));
    assert(!rep::ShouldAwardLiberationFame(0, 0));
    assert(!rep::ShouldAwardLiberationFame(0, 2));
    assert(!rep::ShouldAwardLiberationFame(2, 1));

    const int oldStatus = host.enclaveStatus["Montera"];
    const int newStatus = 0;
    host.enclaveStatus["Montera"] = newStatus;
    if (rep::ShouldAwardLiberationFame(oldStatus, newStatus))
        host.enclaves["Montera"] = rep::LiberationFame;
    for (auto const& [k, v] : host.ReputationSnapshot()) client.ApplyReputationEntry(k, v);
    AssertReputationMatches(host, client);
    assert(client.enclaves["Montera"] == 100);
}

void TestRequestTrackerGating()
{
    rep::RequestTracker tracker{};
    // Connected while !inWorld (pre-admission): must NOT fire EnclaveRequest (net::Send would drop it).
    assert(!rep::UpdateSessionRequest(tracker, true, false, true, 1, 1));
    assert(!tracker.active);

    // Admitted to world: fires once.
    assert(rep::UpdateSessionRequest(tracker, true, true, true, 1, 1));
    assert(tracker.active);
    assert(!rep::UpdateSessionRequest(tracker, true, true, true, 1, 1));

    // Host quick-loads a save (worldEpoch advances 1 -> 2): after re-admission, fires again!
    assert(!rep::UpdateSessionRequest(tracker, true, false, true, 1, 2));
    assert(rep::UpdateSessionRequest(tracker, true, true, true, 1, 2));
    assert(!rep::UpdateSessionRequest(tracker, true, true, true, 1, 2));

    // Reconnect in same worldEpoch (welcomeSerial advances 1 -> 2): fires again!
    assert(rep::UpdateSessionRequest(tracker, true, true, true, 2, 2));
    assert(!rep::UpdateSessionRequest(tracker, true, true, true, 2, 2));

    // Disconnect resets tracker.
    assert(!rep::UpdateSessionRequest(tracker, false, false, true, 2, 2));
    assert(!tracker.active);
}

void TestSplitTheftPerPlayer()
{
    // Host (player 1) is innocent; client 2 steals in KapDun (30 thefts + KnownTheft).
    rep::EnclaveCrimeState hostShared{};
    rep::PlayerTheftState p2{};
    rep::PlayerTheftState p3{};

    // 1. Client 2 steals +17 then +13 (unwitnessed): hostShared must remain 0, p3 remains 0.
    rep::EnclaveCrimeState d1{};
    d1.crime[rep::CrimeTheft] = 17;
    d1.suspect[rep::CrimeTheft] = -1;
    d1.known = -1;
    bool sharedChanged = false, theftChanged = false;
    rep::ApplyClientDeltaOnHost(hostShared, p2, d1, false, sharedChanged, theftChanged);
    assert(!sharedChanged && theftChanged);
    assert(hostShared.crime[rep::CrimeTheft] == 0 && hostShared.known == rep::CrimeNone);
    assert(p2.crimeCount == 17 && !p2.knownTheft);

    // 2. Client 2 gets caught (KnownTheft) and Guard comments on 20 thefts: still isolated to p2.
    rep::EnclaveCrimeState d2{};
    d2.crime[rep::CrimeTheft] = 13;
    d2.suspect[rep::CrimeTheft] = 20;
    d2.known = rep::CrimeTheft;
    rep::ApplyClientDeltaOnHost(hostShared, p2, d2, false, sharedChanged, theftChanged);
    assert(!sharedChanged && theftChanged);
    assert(hostShared.crime[rep::CrimeTheft] == 0);
    assert(hostShared.suspect[rep::CrimeTheft] == 0);
    assert(hostShared.known == rep::CrimeNone);
    assert(p2.crimeCount == 30 && p2.suspectComment == 20 && p2.knownTheft);

    // Projecting EnclaveState to client 2 vs innocent client 3:
    const rep::EnclaveCrimeState viewP2 = rep::ProjectClientEnclaveState(hostShared, p2);
    const rep::EnclaveCrimeState viewP3 = rep::ProjectClientEnclaveState(hostShared, p3);
    assert(viewP2.crime[rep::CrimeTheft] == 30 && viewP2.suspect[rep::CrimeTheft] == 20 && viewP2.known == rep::CrimeTheft);
    assert(viewP3.crime[rep::CrimeTheft] == 0 && viewP3.suspect[rep::CrimeTheft] == 0 && viewP3.known == rep::CrimeNone);

    // 3. Host commits own theft (+25 + KnownTheft): HasSharedNonTheftChange is false and clients stay untouched.
    rep::EnclaveCrimeState hostBefore = hostShared;
    hostShared.crime[rep::CrimeTheft] = 25;
    hostShared.suspect[rep::CrimeTheft] = 20;
    hostShared.known = rep::CrimeTheft;
    assert(!rep::HasSharedNonTheftChange(hostBefore, hostShared, true));
    const rep::EnclaveCrimeState viewP3AfterHostTheft = rep::ProjectClientEnclaveState(hostShared, p3);
    assert(viewP3AfterHostTheft.crime[rep::CrimeTheft] == 0 && viewP3AfterHostTheft.known == rep::CrimeNone);

    // 3b. While Host has KnownTheft (3), Client 3 commits Attack (2): Host keeps KnownTheft locally,
    // while Client 3 (and innocent peers) receive KnownPlayerCrime == Attack (2) instead of dropping it!
    rep::EnclaveCrimeState attackDelta{};
    attackDelta.crime[2] = 1;  // CrimeAttack
    attackDelta.suspect[rep::CrimeTheft] = -1;
    attackDelta.known = 2;     // CrimeAttack
    rep::ApplyClientDeltaOnHost(hostShared, p3, attackDelta, false, sharedChanged, theftChanged);
    assert(sharedChanged && !theftChanged);
    assert(hostShared.known == rep::CrimeTheft && hostShared.nonTheftKnown == 2);
    const rep::EnclaveCrimeState viewP3Attack = rep::ProjectClientEnclaveState(hostShared, p3);
    const rep::EnclaveCrimeState viewP2AttackAndTheft = rep::ProjectClientEnclaveState(hostShared, p2);
    assert(viewP3Attack.crime[2] == 1 && viewP3Attack.known == 2);
    assert(viewP2AttackAndTheft.crime[2] == 1 && viewP2AttackAndTheft.known == rep::CrimeTheft);

    // 4. Shared Murder (4) propagates to all players (4 > 3), while p2 keeps its theft count and p3 keeps 0 thefts.
    rep::EnclaveCrimeState murderDelta{};
    murderDelta.crime[rep::CrimeMurder] = 1;
    murderDelta.suspect[rep::CrimeTheft] = -1;
    murderDelta.known = rep::CrimeMurder;
    rep::ApplyClientDeltaOnHost(hostShared, p3, murderDelta, false, sharedChanged, theftChanged);
    assert(sharedChanged && !theftChanged);
    assert(hostShared.crime[rep::CrimeMurder] == 1 && hostShared.known == rep::CrimeMurder);
    const rep::EnclaveCrimeState viewP3Murder = rep::ProjectClientEnclaveState(hostShared, p3);
    assert(viewP3Murder.crime[rep::CrimeMurder] == 1 && viewP3Murder.crime[rep::CrimeTheft] == 0 && viewP3Murder.known == rep::CrimeMurder);

    // 5. Client 2 clearing its own theft (known=None) does NOT clear host Murder (4).
    rep::EnclaveCrimeState clearP2Theft{};
    clearP2Theft.crime[rep::CrimeTheft] = -30;
    clearP2Theft.suspect[rep::CrimeTheft] = 0;
    clearP2Theft.known = rep::CrimeNone;
    rep::ApplyClientDeltaOnHost(hostShared, p2, clearP2Theft, false, sharedChanged, theftChanged);
    assert(!sharedChanged && theftChanged);
    assert(hostShared.known == rep::CrimeMurder && p2.crimeCount == 0 && p2.suspectComment == 0 && !p2.knownTheft);

    // 6. Native liberation / murder wipe on host detects reset for all players.
    rep::EnclaveCrimeState hostCleared = hostShared;
    hostCleared.crime[rep::CrimeMurder] = 0;
    hostCleared.crime[rep::CrimeTheft] = 0;
    hostCleared.known = rep::CrimeNone;
    assert(rep::HasSharedNonTheftChange(hostShared, hostCleared, true));
    assert(rep::ShouldClearAllPlayerTheftsOnHostReset(hostShared, hostCleared));
}

}  // namespace

int main()
{
    TestWireCodecAndValidation();
    TestPoliticalArraySafety();
    TestJoinReconnectAndHostSaveLoad();
    TestQuestCompletionBothPlayersAndPacketOrders();
    TestDialogReputationAndLiberationAndPersonalMode();
    TestRequestTrackerGating();
    TestSplitTheftPerPlayer();
    return 0;
}
