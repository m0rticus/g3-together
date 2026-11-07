import re
import os
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DOWNED_H = ROOT / "src" / "downed.h"
DOWNED_CPP = ROOT / "src" / "downed.cpp"
SCRIPT_CPP = ROOT / "src" / "Script_G3Together.cpp"


def function_body(source, signature):
    match = re.search(signature, source)
    if not match:
        return None
    opening = source.find("{", match.end())
    if opening < 0:
        return None
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:index]
    return None


class DownedPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.downed_h = DOWNED_H.read_text(encoding="utf-8")
        cls.downed_cpp = DOWNED_CPP.read_text(encoding="utf-8")
        cls.script_cpp = SCRIPT_CPP.read_text(encoding="utf-8")

    def test_die_is_idempotent_and_calls_native_kill_once(self):
        body = function_body(
            self.downed_cpp,
            r"\bvoid\s+Die\s*\(\s*Entity\s*&\s*hero\s*,\s*char\s+const\s*\*\s*reason\s*\)",
        )
        self.assertIsNotNone(body)
        self.assertTrue(body.strip().startswith("if (!life.End())"))
        guard = function_body(body, r"\bif\s*\(\s*!life\.End\(\)\s*\)")
        self.assertIsNotNone(guard)
        self.assertIn('EndStep("skipped-terminal");', guard)
        self.assertIn("return;", guard)
        for native in ("CallScript", "SetHitPoints", "FullStop", "GetInstance"):
            self.assertNotIn(native, guard)
        kill = 'GetScriptAdminExt().CallScript(bCString("Kill"), nullptr, hero.GetInstance(), 0)'
        self.assertEqual(body.count("CallScript("), 1)
        self.assertEqual(body.count(kill), 1)
        self.assertEqual(body.count("terminalKillSent = true;"), 1)
        ordered = (
            'EndStep("decision");',
            'EndStep("set-hp");',
            "hero.PlayerMemory.SetHitPoints(0);",
            'EndStep("full-stop");',
            "hero.Routine.FullStop();",
            "terminalKillSent = true;",
            'EndStep("kill-enter");',
            kill,
            'EndStep("kill-return");',
        )
        self.assertEqual([body.index(step) for step in ordered],
                         sorted(body.index(step) for step in ordered))
        between = body[body.index("terminalKillSent = true;") + len("terminalKillSent = true;"):body.index(kill)]
        self.assertRegex(between, r'^\s*EndStep\("kill-enter"\);\s*(?:const\s+\w+\s+\w+\s*=\s*)?$')
        self.assertNotIn("SetTask", body)

    def test_terminal_tick_latch_never_restarts_a_death_task(self):
        tick = function_body(
            self.downed_cpp,
            r"\bvoid\s+Tick\s*\(\s*\)",
        )
        self.assertIsNotNone(tick)
        start = tick.index("if (life.terminal) {")
        end = tick.index("if (hero.IsDead()", start)
        terminal = tick[start:end]
        self.assertNotIn("SetTask", terminal)
        self.assertNotIn("CallScript", terminal)
        self.assertNotIn("KeepDown", terminal)
        self.assertNotIn("FullStop", terminal)
        self.assertNotIn("StandUp", terminal)
        self.assertNotIn("diedAt", terminal)
        self.assertNotIn("dying again", terminal)
        self.assertIn("hero.PlayerMemory.GetHitPoints() > 0", terminal)
        self.assertIn("hero.PlayerMemory.SetHitPoints(0)", terminal)
        self.assertIn("return;", terminal)

    def test_revive_refuses_terminal_and_world_change_resets_it(self):
        revived = function_body(
            self.downed_cpp,
            r"\bvoid\s+Revived\s*\(\s*int\s+byPlayer\s*,\s*uint32_t\s+sequence\s*\)",
        )
        world_change = function_body(
            self.downed_cpp,
            r"\bvoid\s+OnWorldChange\s*\(\s*\)",
        )
        self.assertIsNotNone(revived)
        guard = function_body(revived, r"\bif\s*\(\s*!helper\b[^{}]*\)")
        self.assertIsNotNone(guard)
        for condition in ("!life.held", "!sequence", "sequence != life.sequence", "life.terminal",
                          "terminalKillSent", "!instance", "!session.IsGameRunning()", "persist::HeroPending()"):
            self.assertIn(condition, revived[:revived.index("{", revived.index("if (!helper"))])
        self.assertIn("return;", guard)
        for condition in ("byPlayer > 0", "byPlayer < net::MaxPlayers", "byPlayer != net::LocalId()"):
            self.assertIn(condition, revived[:revived.index("if (!helper")])
        self.assertLess(revived.index("Able(net::Remotes()[byPlayer], net::Now())"), revived.index("revivedBy = byPlayer;"))
        self.assertLess(revived.index("if (!helper"), revived.index("pendingReviveSequence = sequence;"))
        self.assertIsNotNone(world_change)
        self.assertIn("life.OnWorldChange();", world_change)
        self.assertIn("terminalKillSent = false;", world_change)
        self.assertIn("pendingReviveSequence = 0;", world_change)
        self.assertIn("void OnWorldChange();", self.downed_h)
        self.assertIn("g3t::downed::OnWorldChange();", self.script_cpp)

    def test_native_state_and_loading_cannot_clear_the_downed_latch(self):
        tick = function_body(self.downed_cpp, r"\bvoid\s+Tick\s*\(\s*\)")
        world_change = function_body(self.downed_cpp, r"\bvoid\s+OnWorldChange\s*\(\s*\)")
        self.assertIn("if (!down && !life.held)", tick)
        self.assertNotIn("life.held = false;", world_change)
        self.assertIn("life.OnWorldChange()", world_change)
        self.assertIn('KeepDown(hero, "tick")', tick)
        before_terminal = tick[:tick.index("if (life.terminal)")]
        self.assertNotIn("life.held = false", before_terminal)
        self.assertNotIn("terminalKillSent = false", before_terminal)
        ai = (ROOT / "src" / "ai.cpp").read_text(encoding="utf-8")
        self.assertLess(ai.index("downed::GuardState(self, name)"), ai.index("if (SpectatorState(name))"))
        keys = function_body(ai, r"\bGEInt\s+GE_STDCALL\s+FilterPlayerKeys\s*\(")
        self.assertIn("if (downed::Held()) return 0;", keys)
        self.assertIn("persist::NativeLoading()", self.downed_cpp)
        self.assertIn("if (persist::HeroPending())", tick)
        redirect = function_body(self.downed_cpp, r"\bbool\s+RedirectDeath\s*\(")
        self.assertIn("!persist::HeroPending()", redirect)

    def test_records_preserve_downed_state_before_restoring_hp(self):
        hero = (ROOT / "src" / "hero.cpp").read_text(encoding="utf-8")
        save = function_body(hero, r"\bbool\s+SaveRecordImpl\s*\(")
        load = function_body(hero, r"\bbool\s+LoadRecordImpl\s*\(")
        self.assertIn('fprintf(file, "downed %d\\n"', save)
        self.assertIn('downed::Restore(savedDowned != 0, "hero-record")', load)
        self.assertLess(load.index("downed::Restore"), load.index("self.Inventory.HoldStacks"))
        self.assertIn("downed::Held()", load)

    def test_paid_revive_is_bound_to_sender_and_down_episode(self):
        self.assertIn("sequence != life.sequence", self.downed_cpp)
        self.assertIn("life.Revive(pendingReviveSequence)", self.downed_cpp)
        self.assertIn("hero.IsDown() || hero.IsDead()", self.downed_cpp)
        tick = function_body(self.downed_cpp, r"\bvoid\s+Tick\s*\(\s*\)")
        refused = function_body(tick, r"\bif\s*\(\s*revivedBy\s*&&\s*!Able\(remotes\[revivedBy\],\s*now\)\s*\)")
        self.assertIsNotNone(refused)
        self.assertIn("revivedBy = 0;", refused)
        self.assertIn("pendingReviveSequence = 0;", refused)
        self.assertLess(tick.index("revivedBy && !Able(remotes[revivedBy], now)"),
                        tick.index("life.Revive(pendingReviveSequence)"))
        self.assertIn("life.terminal || terminalKillSent || !life.Revive(pendingReviveSequence)", tick)
        net = (ROOT / "src" / "net.cpp").read_text(encoding="utf-8")
        self.assertIn("message.state.npcClass = from;", net)
        self.assertIn("constexpr uint16_t ProtocolVersion = saving::Protocol;", net)

    def test_client_revive_world_states_and_kind_19_routing(self):
        net = (ROOT / "src" / "net.cpp").read_text(encoding="utf-8")
        send = function_body(net, r"\bvoid\s+SendWorldState\s*\(")
        special = function_body(send, r"\bif\s*\(\s*state\.kind\s*==\s*19\s*\)")
        self.assertIsNotNone(special)
        # The special XP handshake must keep its host broadcast/client direct-send paths and returns.
        self.assertEqual(
            " ".join(special.split()),
            "if (isHost) { if (state.dialog == 0) SendBroadcastExcept(&message, sizeof(message), "
            "ChannelReliable, true, except); return; } "
            "if (connected && server) Send(server, &message, sizeof(message), ChannelReliable, true); return;",
        )
        gate = "if (!host || (!isHost && state.kind != 8 && state.kind != 9)) return;"
        broadcast = "SendBroadcastExcept(&message, sizeof(message), ChannelReliable, true, except);"
        self.assertIn(gate, send)
        self.assertLess(send.index("if (state.kind == 19)"), send.index(gate))
        self.assertLess(send.index(gate), send.rindex(broadcast))
        broadcast_body = function_body(net, r"\bvoid\s+SendBroadcastExcept\s*\(")
        self.assertIn("if (!isHost) { SendToServer(data, size, channel, reliable); return; }", broadcast_body)
        self.assertIn("if (peers[i] && i != except) Send(peers[i], data, size, channel, reliable);", broadcast_body)

        receive = function_body(
            net,
            r"\belse\s+if\s*\(\s*type\s*==\s*MsgWorldState\s*&&\s*"
            r"packet->dataLength\s*==\s*sizeof\(WorldStateMsg\)\s*\)",
        )
        self.assertIsNotNone(receive)
        sanitization = "if (isHost && (message.state.kind == 8 || message.state.kind == 9))"
        self.assertIn("isHost ? static_cast<uint8_t>(reinterpret_cast<uintptr_t>(peer->data)) : 1", receive)
        self.assertIn(sanitization, receive)
        self.assertRegex(receive, re.escape(sanitization) + r"\s*message\.state\.npcClass = from;")
        self.assertLess(receive.index(sanitization), receive.index("message.state.npcClass = from;"))
        self.assertLess(receive.index("message.state.npcClass = from;"), receive.index("worldStateHandler(from, message.state)"))
        special_receive = function_body(receive, r"\bif\s*\(\s*message\.state\.kind\s*==\s*19\s*\)")
        self.assertIn("if (message.state.dialog > 3) return;", special_receive)
        self.assertIn("from < 2 || from >= MaxPlayers || peers[from] != peer || message.state.npcClass != from", special_receive)
        self.assertIn("from != 1 || message.state.npcClass != 1", special_receive)
        infocmd = (ROOT / "src" / "infocmd.cpp").read_text(encoding="utf-8")
        self.assertIn("if (net::IsHost() && s.kind != 3 && s.kind != 16) net::SendWorldState(s, from);", infocmd)
        apply = function_body(infocmd, r"\bvoid\s+Apply\s*\(")
        help_message = function_body(apply, r"\bif\s*\(\s*s\.kind\s*==\s*8\s*\|\|\s*s\.kind\s*==\s*9\s*\)")
        target = function_body(help_message, r"\bif\s*\(\s*s\.guard\s*==\s*net::LocalId\(\)\s*\)")
        self.assertIn("std::memcpy(&sequence, s.guid, sizeof(sequence));", target)
        self.assertIn("if (s.kind == 8) downed::Revived(s.npcClass, sequence);", target)
        self.assertIn("else downed::HelperHasNoPotion(s.npcClass);", target)

    def test_cached_end_step_reaches_crash_handlers(self):
        crash = (ROOT / "src" / "crashlog.cpp").read_text(encoding="utf-8")
        crash_h = (ROOT / "src" / "crashlog.h").read_text(encoding="utf-8")
        end_step = function_body(self.downed_cpp, r"\bvoid\s+EndStep\s*\(")
        self.assertIn("crashlog::EndStep(step);", end_step)
        self.assertIn("downed: end step=%s", end_step)
        transition = function_body(self.downed_cpp, r"\bvoid\s+Transition\s*\(")
        self.assertIn("life.terminal ? 1 : 0", transition)
        self.assertIn("void EndStep(char const* step);", crash_h)
        self.assertIn("std::atomic<char const*> lastEndStep{nullptr};", crash)
        cache = function_body(crash, r"\bvoid\s+EndStep\s*\(")
        report = function_body(crash, r"\bvoid\s+LogEndStep\s*\(")
        self.assertIn("lastEndStep.store(step);", cache)
        self.assertIn("lastEndStep.load()", report)
        self.assertIn("CRASH end-flow last_step=%s", report)
        world_change = function_body(self.downed_cpp, r"\bvoid\s+OnWorldChange\s*\(")
        self.assertIn('crashlog::EndStep("world-change");', world_change)
        for handler in ("OnException", "LastChance"):
            body = function_body(crash, rf"\bLONG\s+(?:CALLBACK|WINAPI)\s+{handler}\s*\(")
            self.assertIsNotNone(body)
            self.assertIn("LogEndStep();", body)

    def test_minidump_fallback_is_limited_and_truncates_before_retry(self):
        crash = (ROOT / "src" / "crashlog.cpp").read_text(encoding="utf-8")
        dump = function_body(crash, r"\bbool\s+WriteDump\s*\(")
        self.assertIsNotNone(dump)
        for flag in ("MiniDumpWithThreadInfo", "MiniDumpWithHandleData", "MiniDumpWithUnloadedModules",
                     "MiniDumpWithDataSegs", "MiniDumpWithIndirectlyReferencedMemory"):
            self.assertIn(flag, dump)
        self.assertEqual(dump.count("writeDump("), 2)
        self.assertLess(dump.index("file, type, exception"), dump.index("DWORD error = GetLastError();"))
        retry = function_body(
            dump,
            r"\bif\s*\(\s*error\s*==\s*ERROR_INVALID_PARAMETER\s*\|\|\s*"
            r"error\s*==\s*static_cast<DWORD>\(E_INVALIDARG\)\s*\)",
        )
        self.assertIsNotNone(retry)
        self.assertLess(retry.index("SetFilePointerEx(file, start, nullptr, FILE_BEGIN)"),
                        retry.index("SetEndOfFile(file)"))
        self.assertLess(retry.index("SetEndOfFile(file)"), retry.index("file, MiniDumpNormal, exception"))
        self.assertIn("crash dump rewind failed", retry)
        self.assertIn("return false;", retry[:retry.index("file, MiniDumpNormal, exception")])
        self.assertIn("error = GetLastError();", retry[retry.index("file, MiniDumpNormal, exception"):])
        self.assertIn("crash dump write failed", dump)
        self.assertIn("WriteDump(file, dumpInfo ? &exception : nullptr);", crash)
        self.assertIn("WriteDump(file, nullptr);", crash)

    def test_engine_free_transition_and_ordering_rules(self):
        build = ROOT / "build" / "downed-policy-tests"
        configure = ["cmake", "-S", str(ROOT / "tests" / "downed_policy"), "-B", str(build)]
        if os.name == "nt":
            configure += ["-A", "Win32"]
        for command in (configure, ["cmake", "--build", str(build), "--config", "Debug"]):
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        executable = build / "Debug" / "downed_policy_test.exe" if os.name == "nt" else build / "downed_policy_test"
        result = subprocess.run([str(executable)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_all_down_and_absence_grace_rules_are_preserved(self):
        all_down = function_body(
            self.downed_cpp,
            r"\bbool\s+AllOthersDown\s*\(\s*net::Remote\s+const\*\s+remotes\s*,\s*uint32_t\s+now\s*\)",
        )
        tick = function_body(
            self.downed_cpp,
            r"\bvoid\s+Tick\s*\(\s*\)",
        )
        self.assertIsNotNone(all_down)
        self.assertIn("Playing(HelperState(r, now))", all_down)
        self.assertIn("if (i == net::LocalId()) continue;", all_down)
        self.assertIn("if (!r.connected) continue;", all_down)
        self.assertIn("if (!Playing(HelperState(r, now))) return false;", all_down)
        self.assertIn("if (HelperState(r, now) != Availability::Down) return false;", all_down)
        self.assertIn("return present;", all_down)
        policy = (ROOT / "src" / "downed_policy.h").read_text(encoding="utf-8")
        self.assertIn("constexpr uint32_t AbsenceGraceMs = 5000;", policy)
        self.assertIsNotNone(tick)
        self.assertIn("AllOthersDown(remotes, now)", tick)
        self.assertIn("DecideEnd(helpPossible, AllOthersDown(remotes, now), now, aloneSince)", tick)
        self.assertIn('Die(hero, "all-down")', tick)
        self.assertIn('Die(hero, "alone-grace")', tick)
        self.assertLess(tick.index("DecideEnd("), tick.index('Die(hero, "all-down")'))
        self.assertLess(tick.index('Die(hero, "all-down")'), tick.index('Die(hero, "alone-grace")'))
        able = function_body(self.downed_cpp, r"\bbool\s+Able\s*\(")
        present = function_body(self.downed_cpp, r"\bbool\s+Present\s*\(")
        self.assertIn("HelperState(r, now) == Availability::Able", able)
        for requirement in ("r.connected", "r.latestTicks", "now - r.latestTicks < StaleState",
                            "r.latest.flags & 1", "r.latest.world == remote::CurrentWorld()", "net::InWorld"):
            self.assertIn(requirement, present)
        helper = function_body(self.downed_cpp, r"\bAvailability\s+HelperState\s*\(")
        for requirement in ("r.connected", "r.activity.known", "r.activity.ticks", "r.activity.flags"):
            self.assertIn(requirement, helper)
        self.assertNotIn("latest", helper)
        self.assertNotIn("world", helper)

    def test_activity_heartbeat_is_authenticated_reliable_and_gate_independent(self):
        net = (ROOT / "src" / "net.cpp").read_text(encoding="utf-8")
        gate = function_body(net, r"\bbool\s+Gated\s*\(")
        self.assertIn("case MsgPlayerActivity:", gate)
        self.assertNotIn("case MsgState:", gate) # presentation/gameplay still gated
        receive = function_body(net, r"\belse\s+if\s*\(\s*type\s*==\s*MsgPlayerActivity\b[^{}]*\)")
        self.assertIn("ENET_PACKET_FLAG_RELIABLE", receive)
        self.assertIn("peers[from] != peer", receive)
        self.assertIn("message.id = static_cast<uint8_t>(from)", receive)
        self.assertIn("peer != server || !connected", receive)
        self.assertIn("r.activity.Accept", receive)
        self.assertNotIn("Store(", receive)
        self.assertNotIn("SetInWorld", receive)
        # Forward valid heartbeat even if a newer pose already updated the host's activity.
        self.assertNotIn("if (!r.activity.Accept", receive)
        self.assertIn("ChannelState, true", receive) # independent of the bulk NPC/control reliable channel
        service = function_body(net, r"\bvoid\s+Service\s*\(")
        self.assertLess(service.index("MsgPlayerActivity"), service.index("if (!haveLocal"))
        self.assertIn("now - lastActivitySend >= 1000", service)
        self.assertIn("local.downedSequence != lastActivityDowned", service)
        self.assertIn("local.downedSequence != currentLocal.downedSequence", service)
        self.assertIn("local.actionSeq != currentLocal.actionSeq", service)

    def test_world_gate_clears_only_presentation_disconnect_clears_activity(self):
        net = (ROOT / "src" / "net.cpp").read_text(encoding="utf-8")
        clear = function_body(net, r"\bvoid\s+ClearPresentation\s*\(")
        self.assertIn("cleared.activity = remotes[id].activity", clear)
        self.assertIn("cleared.connected = remotes[id].connected", clear)
        gate = function_body(net, r"\bvoid\s+SetInWorld\s*\(")
        self.assertIn("ClearPresentation(i)", gate)
        self.assertIn("ClearPresentation(id)", gate)
        drop = function_body(net, r"\bvoid\s+Drop\s*\(")
        self.assertIn("remotes[id] = Remote{}", drop)

    def test_arriving_held_puppet_uses_owner_life_not_buffered_wake(self):
        remote = (ROOT / "src" / "remote.cpp").read_text(encoding="utf-8")
        update = function_body(remote, r"\bvoid\s+UpdatePuppet\s*\(")
        held = function_body(update, r"\bif\s*\(\s*held\s*\)")
        for requirement in ("target.position = remote.latest.position", "target.speed = 0.0f",
                            "target.movementMode = gECharMovementMode_1",
                            "target.aniState = gEAniState_LieKnockDown",
                            "world::RequestProcessingUpdate()", 'std::strstr(target.motion, "GetUp")'):
            self.assertIn(requirement, held)
        self.assertIn("puppet.entity.Routine.AniState = gEAniState_LieKnockDown", update)
        self.assertIn("|| heldOwner", update)
        self.assertIn("remote.activity.downedSequence == remote.latest.downedSequence", update)
        self.assertIn("puppet.entity.Dialog.TradeEnabled", update)
        self.assertNotIn('CallScript(bCString("Kill")', update)
        pay = function_body(self.downed_cpp, r"\bvoid\s+PayAndHelp\s*\(")
        self.assertIn("paidSequence[i] == r.latest.downedSequence && now - paidAt[i] < PaidCooldown", pay)

    def test_absence_boundaries_log_candidate_rejection_evidence(self):
        tick = function_body(self.downed_cpp, r"\bvoid\s+Tick\s*\(")
        for event in ("alone-start", "alone-cancel", "alone-expiry"):
            self.assertIn(f'LogHelpers(&hero, "{event}", now)', tick)
        die = function_body(self.downed_cpp, r"\bvoid\s+Die\s*\(")
        self.assertIn("LogHelpers(&hero, reason, now)", die)
        cancel = function_body(self.downed_cpp, r"\bvoid\s+CancelAlone\s*\(\s*char\s+const\s*\*\s*source\s*\)\s*(?=\{)")
        self.assertIn('LogHelpers(nullptr, "alone-cancel", now)', cancel)
        self.assertNotIn("GetInstance", cancel)
        world_change = function_body(self.downed_cpp, r"\bvoid\s+OnWorldChange\s*\(")
        self.assertIn('CancelAlone("world-change")', world_change)
        log = function_body(self.downed_cpp, r"\bvoid\s+LogHelpers\s*\(")
        for field in ("connected=", "joined=", "gate=", "activity_age=", "pose_age=", "loading=", "menu=",
                      "partner_world=", "partner_epoch=", "distance_cm=", "zone=not-sent", "reject="):
            self.assertIn(field, log)


if __name__ == "__main__":
    unittest.main()
