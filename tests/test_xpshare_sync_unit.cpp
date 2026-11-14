#include "../src/xpshare.h"

#include <cassert>
#include <cstdint>

int main()
{
    using g3t::xpshare::counter::BaselineEarned;
    using g3t::xpshare::counter::BaselineRetryMs;
    using g3t::xpshare::counter::HeartbeatMs;
    using g3t::xpshare::counter::OutboundPlan;
    using g3t::xpshare::counter::PeerLink;
    using g3t::xpshare::counter::Receiver;
    using g3t::xpshare::counter::SharesNativeSource;

    constexpr uint32_t maximum = 10000000;

    // 1. Pre-ACK XP earn immediately plans Baseline + Live in one ordered batch,
    //    delivers XP in one one-way trip, and never double-pays on the later ACK.
    {
        PeerLink sender;
        Receiver receiver;
        uint32_t earned = 400;

        assert(sender.SyncGeneration(true, 7));
        sender.wasInWorld = true;
        sender.outgoing.Begin(101, earned);
        sender.ResetAdmissionHandshake();

        // Initial baseline send fails (e.g. hero not bound yet): retry stays immediate, not 2000 ms.
        assert(sender.ShouldSendBaseline(1000));
        assert(sender.ShouldSendBaseline(1016));
        sender.MarkBaselineSent(1016);
        assert(!sender.ShouldSendBaseline(1032));
        assert(!sender.ShouldSendBaseline(1016 + BaselineRetryMs - 1));
        assert(sender.ShouldSendBaseline(1016 + BaselineRetryMs));

        // XP earned at t=1032 BEFORE ACK arrives: PlanLive immediately emits Baseline + Live!
        earned += 75;
        const OutboundPlan preAck = sender.PlanLive(earned, 1032, false);
        assert(preAck.sendBaseline && preAck.sendLive);
        sender.MarkLiveSent(earned, 1032, preAck.sendBaseline);

        // Next 60 Hz frame while ACK is in flight does not spam duplicate Baseline + Live.
        const OutboundPlan nextFrame = sender.PlanLive(earned, 1048, false);
        assert(!nextFrame.sendBaseline && !nextFrame.sendLive);

        // Receiver gets Baseline + Live in order on ChannelReliable: applies +75 immediately!
        assert(receiver.Snapshot(sender.outgoing.nonce, BaselineEarned));
        assert(receiver.Take(sender.outgoing.nonce, sender.outgoing.Total(earned), maximum) == 75);

        // ACK arrives at t=1052: sender sends one acked confirmation Live; receiver applies 0 duplicate XP.
        sender.NoteAck(101);
        const OutboundPlan onAck = sender.PlanLive(earned, 1052, false);
        assert(!onAck.sendBaseline && onAck.sendLive);
        sender.MarkLiveSent(earned, 1052, onAck.sendBaseline);
        assert(receiver.Take(sender.outgoing.nonce, sender.outgoing.Total(earned), maximum) == 0);
        assert(receiver.applied == 75);

        // Quiet after acked confirmation until either new XP or HeartbeatMs.
        const OutboundPlan steady = sender.PlanLive(earned, 1068, false);
        assert(!steady.sendBaseline && !steady.sendLive);
        const OutboundPlan heartbeat = sender.PlanLive(earned, 1052 + HeartbeatMs, true);
        assert(!heartbeat.sendBaseline && heartbeat.sendLive);
    }

    // 2. If pre-ACK Live was gated at world entry (only Baseline arrived), ACK re-sends Live and delivers XP.
    {
        PeerLink sender;
        Receiver receiver;
        uint32_t earned = 100;

        assert(sender.SyncGeneration(true, 3));
        sender.wasInWorld = true;
        sender.outgoing.Begin(202, earned);
        sender.ResetAdmissionHandshake();

        earned += 50;
        const OutboundPlan preAck = sender.PlanLive(earned, 500, false);
        assert(preAck.sendBaseline && preAck.sendLive);
        sender.MarkLiveSent(earned, 500, preAck.sendBaseline);

        // Simulate receiver accepting Baseline via XpHandshake while Live was dropped before Open(peer).
        assert(receiver.Snapshot(sender.outgoing.nonce, BaselineEarned));
        assert(receiver.applied == 0);

        // ACK reaches sender: PlanLive still sees ackedSentEarned != earned and re-sends Live!
        sender.NoteAck(202);
        const OutboundPlan afterAck = sender.PlanLive(earned, 520, false);
        assert(!afterAck.sendBaseline && afterAck.sendLive);
        sender.MarkLiveSent(earned, 520, afterAck.sendBaseline);
        assert(receiver.Take(sender.outgoing.nonce, sender.outgoing.Total(earned), maximum) == 50);
    }

    // 3. SyncGeneration on OnState(Baseline) prevents a subsequent PrepareAdmissions on the same
    //    generation from wiping an already-recorded peer baseline.
    {
        PeerLink clientPeer;
        assert(clientPeer.SyncGeneration(true, 12));
        assert(clientPeer.Snapshot(303, BaselineEarned));
        assert(clientPeer.known && clientPeer.nonce == 303);

        // PrepareAdmissions runs later in the same frame with the same WelcomeSerial (12): must keep Snapshot!
        assert(clientPeer.SyncGeneration(true, 12));
        assert(clientPeer.known && clientPeer.nonce == 303);
        assert(clientPeer.Take(303, 60, maximum) == 60);

        // Reconnect with a new generation (13) resets cleanly.
        assert(clientPeer.SyncGeneration(true, 13));
        assert(!clientPeer.known && clientPeer.nonce == 0 && clientPeer.applied == 0);
    }

    // 4. Request recovery re-plans Baseline + Live immediately without double-paying.
    {
        PeerLink sender;
        Receiver resetReceiver;
        uint32_t earned = 0;

        assert(sender.SyncGeneration(true, 1));
        sender.wasInWorld = true;
        sender.outgoing.Begin(404, earned);
        sender.NoteAck(404);

        earned += 120;
        const OutboundPlan live = sender.PlanLive(earned, 2000, false);
        assert(!live.sendBaseline && live.sendLive);
        sender.MarkLiveSent(earned, 2000, false);

        // Receiver missed baseline (e.g. OnWorldChange race) and sends Request:
        sender.ResetAdmissionHandshake();
        const OutboundPlan recovery = sender.PlanLive(earned, 2010, false);
        assert(recovery.sendBaseline && recovery.sendLive);
        sender.MarkLiveSent(earned, 2010, recovery.sendBaseline);

        assert(resetReceiver.Snapshot(404, BaselineEarned));
        assert(resetReceiver.Take(404, sender.outgoing.Total(earned), maximum) == 120);
        assert(resetReceiver.Take(404, sender.outgoing.Total(earned), maximum) == 0);
    }

    // 5. World_MCP quest XP is excluded from xpshare (replicated natively by story.cpp),
    //    while non-world GiveXP sources share normally; host relays client gains to other peers only.
    {
        assert(!SharesNativeSource(true));
        assert(SharesNativeSource(false));

        PeerLink toClientA, toClientB, lateJoinerC;
        Receiver clientAReceiver, clientBReceiver, clientCReceiver;
        uint32_t hostEarned = 300;

        toClientA.wasInWorld = true;
        toClientA.outgoing.Begin(501, hostEarned);
        toClientA.NoteAck(501);
        toClientA.MarkLiveSent(hostEarned, 2900, false);
        assert(clientAReceiver.Snapshot(501, BaselineEarned));

        toClientB.wasInWorld = true;
        toClientB.outgoing.Begin(502, hostEarned);
        toClientB.NoteAck(502);
        toClientB.MarkLiveSent(hostEarned, 2900, false);
        assert(clientBReceiver.Snapshot(502, BaselineEarned));

        // Client A earns +80 XP and sends it to Host: Host relays +80 to Client B, not back to Client A.
        toClientB.outgoing.Relay(80);
        assert(!toClientA.PlanLive(hostEarned, 3000, false).sendLive);
        const OutboundPlan relayPlan = toClientB.PlanLive(hostEarned, 3000, false);
        assert(!relayPlan.sendBaseline && relayPlan.sendLive);
        toClientB.MarkLiveSent(hostEarned, 3000, false);
        assert(clientBReceiver.Take(502, toClientB.outgoing.Total(hostEarned), maximum) == 80);
        assert(clientAReceiver.Take(501, toClientA.outgoing.Total(hostEarned), maximum) == 0);

        // Late-joining Client C starts a fresh stream with relayed == 0 (no pre-join leak).
        lateJoinerC.wasInWorld = true;
        lateJoinerC.outgoing.Begin(503, hostEarned);
        assert(clientCReceiver.Snapshot(503, BaselineEarned));
        assert(clientCReceiver.Take(503, lateJoinerC.outgoing.Total(hostEarned), maximum) == 0);
    }

    return 0;
}
