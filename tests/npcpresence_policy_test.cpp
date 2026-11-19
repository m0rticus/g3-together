// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/npcpresence_policy.h"
#include "../src/npclease.h"

#include <cassert>

using namespace g3t::npcpresence;

int main()
{
    // 1. Unmirrored template NPC on client (host counterpart fled out of 80m sphere or died out of view) is Hidden.
    Facts unmirrored{};
    unmirrored.character = true;
    assert(Decide(unmirrored) == Verdict::Hidden);

    // 2. Host world entities, players/puppets, and non-character objects remain Present unless removed/quarantined.
    Facts onHost = unmirrored;
    onHost.isHost = true;
    assert(Decide(onHost) == Verdict::Present);
    onHost.removed = true;
    assert(Decide(onHost) == Verdict::Hidden);

    Facts puppet = unmirrored;
    puppet.playerOrPuppet = true;
    assert(Decide(puppet) == Verdict::Present);

    Facts staticProp{};
    staticProp.character = false;
    assert(Decide(staticProp) == Verdict::Present);

    // 3. Newly bound mirror stays Hidden until PassiveMirrorTick places it at host coordinates (zero 1-frame template flash),
    // then becomes Present immediately once placed.
    Facts mirror = unmirrored;
    mirror.hostMirrorBound = true;
    mirror.hostMirrorPlaced = false;
    assert(Decide(mirror) == Verdict::Hidden);
    mirror.hostMirrorPlaced = true;
    assert(Decide(mirror) == Verdict::Present);

    // 4. Released live mirror stays Present while final release events drain, then hides so it never freezes as a ghost,
    // and reversibly reappears when the host streams it again.
    mirror.hostMirrorReleased = true;
    mirror.hostReleasePending = true;
    assert(Decide(mirror) == Verdict::Present);
    mirror.hostReleasePending = false;
    assert(Decide(mirror) == Verdict::Hidden);
    mirror.hostMirrorReleased = false;
    assert(Decide(mirror) == Verdict::Present);

    // 5. Dead mirrors and settled corpses stay Present even after stream release, unless void-despawned via Respawn.
    Facts deadMirror = mirror;
    deadMirror.hostMirrorDead = true;
    deadMirror.hostMirrorReleased = true;
    assert(Decide(deadMirror) == Verdict::Present);
    deadMirror.despawned = true;
    assert(Decide(deadMirror) == Verdict::Hidden);
    // Reincarnation clears despawned and restores live presence.
    deadMirror.despawned = false;
    deadMirror.hostMirrorDead = false;
    deadMirror.hostMirrorReleased = false;
    assert(Decide(deadMirror) == Verdict::Present);

    Facts settledCorpse = unmirrored;
    settledCorpse.hostCorpseBound = true;
    assert(Decide(settledCorpse) == Verdict::Present);
    settledCorpse.despawned = true;
    assert(Decide(settledCorpse) == Verdict::Hidden);

    // 6. Client lease stays Hidden while provisional/unvetted (or after Kind::Reject), becomes Present once vetted by
    // host Offer/Commit, and stays blink-free during cut-to-host handback.
    Facts leased = unmirrored;
    leased.clientLeaseVetted = false;
    assert(Decide(leased) == Verdict::Hidden);
    leased.clientLeaseVetted = true;
    assert(Decide(leased) == Verdict::Present);
    leased.clientLeaseVetted = false;
    leased.clientHandbackPending = true;
    assert(Decide(leased) == Verdict::Present);
    leased.clientHandbackPending = false;
    assert(Decide(leased) == Verdict::Hidden);

    // 6b. Active client dialogue partners and transitioning FollowNpc guides stay Present across authority handoffs,
    // without flashing never-placed template ghosts or overriding void-despawn / removal.
    Facts dialogPartner = unmirrored;
    dialogPartner.clientDialogPartner = true;
    assert(Decide(dialogPartner) == Verdict::Present);
    dialogPartner.despawned = true;
    assert(Decide(dialogPartner) == Verdict::Hidden);

    Facts guideTransition = unmirrored;
    guideTransition.clientGuideActive = true;
    // Never-placed template entity waits for host placement before appearing.
    assert(Decide(guideTransition) == Verdict::Hidden);
    // Previously vetted client lease relinquished to host for guiding stays Present while host stream warms up.
    guideTransition.clientPreviouslyVetted = true;
    assert(Decide(guideTransition) == Verdict::Present);
    // Placed host mirror actively guiding the client stays Present even across brief stream release.
    Facts placedGuide = unmirrored;
    placedGuide.clientGuideActive = true;
    placedGuide.hostMirrorBound = true;
    placedGuide.hostMirrorPlaced = true;
    placedGuide.hostMirrorReleased = true;
    assert(Decide(placedGuide) == Verdict::Present);
    // Once guiding ends and the player walks away after stream release drains, normal hiding resumes.
    placedGuide.clientGuideActive = false;
    assert(Decide(placedGuide) == Verdict::Hidden);

    // 7. Void despawn coordinate classifier and Offer baseline drift gate.
    const float voidPos[3] = {-220000.0f, 100000.0f, 100000.0f};
    const float nearVoid[3] = {-219500.0f, 100250.0f, 99800.0f};
    const float ardeaDeer[3] = {-117230.0f, 3810.0f, -64210.0f};
    const float fledDeer[3] = {-102230.0f, 3920.0f, -51210.0f};
    assert(IsVoidDespawnPosition(voidPos));
    assert(IsVoidDespawnPosition(nearVoid));
    assert(!IsVoidDespawnPosition(ardeaDeer));
    assert(!IsVoidDespawnPosition(nullptr));

    assert(OfferMatchesLocalBaseline(ardeaDeer, 120, 0, ardeaDeer, 120, 0));
    const float slightStep[3] = {-117180.0f, 3810.0f, -64190.0f};
    assert(OfferMatchesLocalBaseline(ardeaDeer, 120, 0, slightStep, 120, 0));
    assert(!OfferMatchesLocalBaseline(ardeaDeer, 120, 0, fledDeer, 120, 0));
    assert(!OfferMatchesLocalBaseline(ardeaDeer, 120, 0, ardeaDeer, 80, 0));
    assert(!OfferMatchesLocalBaseline(ardeaDeer, 120, 0, ardeaDeer, 0, 1));
    assert(OfferMatchesLocalBaseline({ardeaDeer, 120, 0, 1, 0, 0, 0}, {ardeaDeer, 120, 0, 1, 0, 0, 0}));
    assert(!OfferMatchesLocalBaseline({ardeaDeer, 120, 0, 3, 0, 0, 0}, {ardeaDeer, 120, 0, 1, 0, 0, 0}));
    assert(!OfferMatchesLocalBaseline({ardeaDeer, 120, 0, 1, 3, 0, 0}, {ardeaDeer, 120, 0, 1, 0, 0, 0}));
    assert(!OfferMatchesLocalBaseline({ardeaDeer, 120, 0, 1, 0, 9, 0}, {ardeaDeer, 120, 0, 1, 0, 0, 0}));
    assert(!OfferMatchesLocalBaseline({ardeaDeer, 120, 0, 1, 0, 0, 1}, {ardeaDeer, 120, 0, 1, 0, 0, 0}));

    // 8. Kind::Reject wire message passes Valid() and round-trips without requiring a full Checkpoint payload.
    g3t::npclease::Message reject{};
    reject.kind = g3t::npclease::Kind::Reject;
    reject.world = 3;
    reject.owner = 2;
    reject.incarnation = 1;
    reject.echo = 4200;
    reject.guid[0] = 0x5a;
    assert(g3t::npclease::Valid(reject));
    std::vector<uint8_t> wire;
    assert(g3t::npclease::EncodeMessage(reject, wire));
    g3t::npclease::Message decoded{};
    assert(g3t::npclease::DecodeMessage(wire.data(), wire.size(), decoded));
    assert(decoded.kind == g3t::npclease::Kind::Reject);
    assert(decoded.world == 3 && decoded.owner == 2 && decoded.incarnation == 1 && decoded.echo == 4200);
    assert(decoded.guid[0] == 0x5a);
}
