// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/npclease_presentation.h"
#include "../src/npclease_client.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <cmath>

int main()
{
    using g3t::npclease::PresentationFresh;
    assert(PresentationFresh(true, false, 0, 100));
    assert(PresentationFresh(true, false, 400, 100));
    assert(PresentationFresh(true, false, 404, 100));
    assert(PresentationFresh(true, false, 500, 100));
    assert(!PresentationFresh(true, false, 501, 100));
    assert(!PresentationFresh(true, true, 0, 100));
    assert(!PresentationFresh(false, false, 0, 100));
    assert(!PresentationFresh(true, false, UINT32_MAX, 0));
    assert(PresentationFresh(true, false, 350, 16));
    assert(!PresentationFresh(true, false, 351, 16));
    assert(PresentationFresh(true, false, 1000, 250));
    assert(PresentationFresh(true, false, 1100, 250));
    assert(!PresentationFresh(true, false, 1101, 250));
    using namespace g3t::npclease;
    assert(CharacterPresentation("Slave_04", true, true, true, true));
    assert(CharacterPresentation("Wolf", true, true, true, true));
    assert(!CharacterPresentation("G3_Object_Interact_Innos_Statue_01", true, true, true, true));
    assert(!CharacterPresentation("Door", false, true, true, true));
    assert(!CharacterPresentation("Statue", true, true, false, true));
    assert(!CharacterPresentation("Item", true, false, true, true));
    assert(!CharacterPresentation("Dummy", true, true, true, false));
    assert(!CharacterPresentation(nullptr, true, true, true, true));
    assert(std::fabs(IdleTime(0, 4.0f) - 1.0f) < 0.001f);
    assert(std::fabs(IdleTime(500, 4.0f) - 1.5f) < 0.001f);
    assert(std::fabs(IdleTime(4000, 4.0f) - 1.0f) < 0.001f);
    assert(IdleTime(1, 0.0f) == 0.0f);
    assert(IdleTime(1, INFINITY) == 0.0f);
    assert(ReactionTime(0, 2.0f) == 0.0f);
    assert(std::fabs(ReactionTime(500, 2.0f) - 0.5f) < 0.001f);
    assert(ReactionTime(3000, 2.0f) < 2.0f && ReactionTime(3000, 2.0f) > 1.99f);
    assert(ReactionTime(1, 0.0f) == 0.0f);
    assert(ReactionTime(1, INFINITY) == 0.0f);
    assert(CosmeticContact(true, true, false, true, false, false, 1));
    assert(!CosmeticContact(false, true, false, true, false, false, 1));
    assert(!CosmeticContact(true, false, false, true, false, false, 1));
    assert(!CosmeticContact(true, true, true, true, false, false, 1));
    assert(!CosmeticContact(true, true, false, false, false, false, 1));
    assert(!CosmeticContact(true, true, false, true, true, false, 1));
    assert(!CosmeticContact(true, true, false, true, false, true, 1));
    assert(!CosmeticContact(true, true, false, true, false, false, 0));
    int actor, instance, nodes, replacement;
    PresentationRig rig{&actor, &instance, &nodes, 20, false};
    PresentationProof proof;
    assert(!proof.Usable(rig)); // Encodable bind bones and playback initialization are not proof.
    bool initialized = true;
    assert(initialized && !proof.Usable(rig));
    proof.Record(rig, PoseProvenance::Canonical, false, true);
    assert(!proof.Usable(rig));
    proof.Record(rig, PoseProvenance::Canonical, true, false);
    assert(!proof.Usable(rig));
    proof.Record(rig, PoseProvenance::Canonical, true, true);
    assert(proof.Usable(rig));
    auto changed = rig;
    changed.instance = &replacement;
    assert(!proof.Usable(changed));
    changed = rig; changed.nodes = &replacement;
    assert(!proof.Usable(changed));
    changed = rig; changed.actor = &replacement;
    assert(!proof.Usable(changed));
    changed = rig; ++changed.count;
    assert(!proof.Usable(changed));
    changed = rig; changed.facial = true;
    assert(!proof.Usable(changed));
    proof.Record(changed, PoseProvenance::Idle, true, true);
    assert(proof.Usable(changed));
    proof.Record(changed, PoseProvenance::Native, true, true);
    assert(proof.Usable(changed));
    proof.Record(changed, PoseProvenance::Reaction, true, true);
    assert(proof.Usable(changed));
    proof.Record(changed, PoseProvenance::Canonical, true, true);
    assert(proof.Usable(changed) && proof.provenance == PoseProvenance::Canonical);
    proof.Record(changed, PoseProvenance::Idle, true, false);
    assert(!proof.Usable(changed));
    Checkpoint discovered;
    discovered.templateId[0] = 1;
    std::strcpy(discovered.name, "Slave_04");
    std::strcpy(discovered.routine, "ZS_Routine");
    discovered.hp = discovered.maxHp = 100;
    Message adoption;
    adoption.kind = Kind::Adopt; adoption.world = 1; adoption.owner = 2;
    adoption.guid[0] = 3; adoption.state = discovered; adoption.echo = 100;
    std::vector<uint8_t> wire;
    assert(EncodeMessage(adoption, wire));
    Message decoded;
    assert(DecodeMessage(wire.data(), wire.size(), decoded));
    assert(decoded.kind == Kind::Adopt && decoded.state.hp == 100);

    Lease record;
    ClientWindow client;
    assert(record.Offer(2, 7, 10, 100, decoded.state));
    assert(record.ActiveLeases()); // host fence exists before any client can commit
    assert(client.Prepare(10, 100, LeaseMs, 110));
    assert(!client.Active(10, 110));
    assert(record.Prepared(10, 120));
    assert(client.Commit(10, 130));
    Checkpoint latest = discovered;
    latest.position[0] = 1234; latest.hp = 25;
    assert(record.AcceptState(2, 7, 10, 1, 1, latest));
    assert(!record.AcceptState(2, 6, 10, 1, 2, discovered));
    assert(!record.AcceptState(2, 7, 10, 1, 0, discovered));
    assert(!record.AcceptState(2, 7, 10, 1, 1, latest));
    assert(CommitEffect(latest, NativeDeath));
    latest.hp = 0;
    assert(!CommitEffect(latest, NativeDeath));
    assert(record.Cut(2, 7, 10, 1, 2, latest));
    client.Freeze();
    assert(!client.Active(10, 140));
    assert(CommitEffect(record.state, QuestDeath));
    assert(!CommitEffect(record.state, QuestDeath));
    assert(record.HandBack(11, 150));
    assert(record.owner == 1 && record.state.position[0] == 1234 && record.state.dead);
    assert(!record.AcceptState(2, 7, 10, 1, 3, discovered));

    Lease interrupted;
    ClientWindow lateClient;
    assert(interrupted.Offer(2, 7, 12, 200, discovered));
    assert(lateClient.Prepare(12, 200, LeaseMs, 210));
    assert(interrupted.Recover(200 + PrepareMs));
    assert(!lateClient.Commit(12, 200 + PrepareMs));
    assert(interrupted.owner == 1);
    Lease disconnected;
    assert(disconnected.Offer(2, 8, 13, 4000, discovered));
    assert(disconnected.Prepared(13, 4010));
    latest = discovered; latest.hp = 20;
    assert(disconnected.AcceptState(2, 8, 13, 1, 1, latest));
    assert(disconnected.Recover(4010 + LeaseMs + LeaseSafetyMs));
    assert(disconnected.phase == Phase::Dormant && disconnected.state.hp == 20);
    assert(!disconnected.AcceptState(2, 8, 13, 1, 2, discovered));
    std::cout << "Immediate release, stale presentation and missing-driver policy passed\n";
    std::cout << "Discovered-record transfer, terminal cut, duplicate ledger and timeout model passed\n";
}
