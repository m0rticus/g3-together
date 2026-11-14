import re
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

TARGET = _testbuild.register_target(
    "downed_policy", [ROOT / "tests" / "downed_policy" / "downed_policy_test.cpp"],
    warnings="none",
)
DOWNED_H = ROOT / "src" / "downed.h"
DOWNED_CPP = ROOT / "src" / "downed.cpp"
SCRIPT_CPP = ROOT / "src" / "Script_G3Together.cpp"


SPLIT_SIBLINGS = (
    (ROOT / "src" / "downed.cpp", (ROOT / "src" / "downed_terminal.cpp", ROOT / "src" / "player_immunity.cpp")),
    (ROOT / "src" / "combat.cpp", (ROOT / "src" / "combat_melee.cpp", ROOT / "src" / "combat_claims.cpp")),
    (ROOT / "src" / "ai.cpp", (ROOT / "src" / "npc_targets.cpp", ROOT / "src" / "patch.cpp")),
)


def _extract_function_body(source, signature):
    for match in re.finditer(signature, source):
        opening = source.find("{", match.end())
        if opening < 0:
            return None
        if source.find(";", match.end(), opening) >= 0:
            continue
        depth = 0
        for index in range(opening, len(source)):
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
                if depth == 0:
                    return source[opening + 1:index]
    return None


def function_body(source, signature):
    body = _extract_function_body(source, signature)
    if body is not None:
        return body
    for primary, siblings in SPLIT_SIBLINGS:
        if primary.exists() and source == _testbuild.cached_text(primary):
            for sibling in siblings:
                if sibling.exists():
                    body = _extract_function_body(_testbuild.cached_text(sibling), signature)
                    if body is not None:
                        return body
    return None


class DownedPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.downed_h = _testbuild.cached_text(DOWNED_H)
        cls.downed_cpp = _testbuild.cached_text(DOWNED_CPP)
        cls.downed_all = "\n".join(
            _testbuild.cached_text(p)
            for p in (
                DOWNED_CPP,
                ROOT / "src" / "downed_terminal.cpp",
                ROOT / "src" / "player_immunity.cpp",
            )
        )
        cls.script_cpp = _testbuild.cached_text(SCRIPT_CPP)

    def test_die_is_idempotent_and_dispatches_native_ragdoll_death_once(self):
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
        self.assertNotIn("CallScript", body)
        self.assertNotIn('bCString("Kill")', body)
        self.assertEqual(body.count('bCString("ZS_RagDollDead")'), 1)
        self.assertEqual(body.count("routine->AISetTask"), 1)
        self.assertEqual(body.count("ai::EnterTerminalDeath"), 1)
        self.assertIn("hero.Routine.AIMode = gEAIMode_Reaction;", body)
        self.assertIn("hero.CharacterMovement.StopMovement();", body)
        self.assertIn("Entity clearAttacker;", body)
        self.assertIn("Entity clearInflictor;", body)
        self.assertIn("hero.NPC.SetCurrentAttacker(clearAttacker);", body)
        self.assertIn("hero.DamageReceiver.SetLastInflictor(clearInflictor);", body)
        self.assertIn("attacker_clear=%d inflictor_clear=%d", body)
        self.assertIn("force=no-force", body)
        self.assertLess(body.index("hero.CharacterMovement.StopMovement();"),
                        body.index("hero.NPC.SetCurrentAttacker(clearAttacker);"))
        self.assertLess(body.index("hero.DamageReceiver.SetLastInflictor(clearInflictor);"),
                        body.index('routine->AISetTask(bCString("ZS_RagDollDead"));'))
        ordered = (
            'EndStep("decision");',
            'EndStep("set-hp");',
            "hero.PlayerMemory.SetHitPoints(0);",
            'EndStep("death-full-stop");',
            "hero.Routine.FullStop();",
            "hero.CharacterMovement.StopMovement();",
            "hero.NPC.SetCurrentAttacker(clearAttacker);",
            "hero.DamageReceiver.SetLastInflictor(clearInflictor);",
            "hero.Routine.AIMode = gEAIMode_Reaction;",
            'bCString("ZS_RagDollDead")',
            'EndStep("death-state-dispatch");',
            "ai::EnterTerminalDeath(hero.GetInstance())",
        )
        self.assertEqual([body.index(step) for step in ordered],
                         sorted(body.index(step) for step in ordered))

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
        self.assertIn('KeepTerminal(hero, "Tick-terminal")', terminal)
        repair = function_body(self.downed_cpp, r"\bvoid\s+KeepTerminal\s*\(")
        self.assertIn("hero.PlayerMemory.SetHitPoints(0)", repair)
        self.assertIn("if (!life.deathFinished) return;", repair)
        self.assertIn("hero.Routine.AIMode = gEAIMode_Dead", repair)
        self.assertIn('routine->SetCurrentTask(bCString("ZS_RagDollDead"))', repair)
        self.assertIn('routine->AISetState(bCString("ZS_RagDollDead_Loop"))', repair)
        self.assertNotIn("AISetTask", repair)
        self.assertNotIn("EnterTerminalDeath", repair)
        self.assertNotIn("FullStop", repair)
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
                          "!instance", "!session.IsGameRunning()", "persist::HeroPending()"):
            self.assertIn(condition, revived[:revived.index("{", revived.index("if (!helper"))])
        self.assertIn("return;", guard)
        for condition in ("byPlayer > 0", "byPlayer < net::MaxPlayers", "byPlayer != net::LocalId()"):
            self.assertIn(condition, revived[:revived.index("if (!helper")])
        self.assertLess(revived.index("Able(net::Remotes()[byPlayer], net::Now())"), revived.index("revivedBy = byPlayer;"))
        self.assertLess(revived.index("if (!helper"), revived.index("pendingReviveSequence = sequence;"))
        self.assertIsNotNone(world_change)
        self.assertIn("life.OnWorldChange();", world_change)
        self.assertIn("nativeTerminalKills = 0;", world_change)
        self.assertIn("nativeTerminalKillChecked = false;", world_change)
        self.assertIn("terminalTask.clear();", world_change)
        self.assertIn("pendingReviveSequence = 0;", world_change)
        self.assertIn("void OnWorldChange();", self.downed_h)
        self.assertIn("bool Terminal();", self.downed_h)
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
        ai = _testbuild.src_text("ai.cpp")
        self.assertLess(ai.index("downed::GuardState(self, name)"), ai.index("if (SpectatorState(name))"))
        self.assertIn('std::strcmp(name, "ZS_RagDollDead") == 0', ai)
        self.assertIn('downed::ObserveDeathState(self, true)', ai)
        self.assertIn('downed::ObserveDeathState(self, false, result != GEFalse)', ai)
        self.assertNotIn("ObserveNativeKill", ai)
        self.assertNotIn('std::strcmp(name, "Kill")', ai)
        self.assertNotIn("nativeTerminalKill", ai)
        observe = function_body(
            self.downed_cpp,
            r"\bbool\s+ObserveDeathState\s*\(\s*eCEntity\s*\*\s*self\s*,\s*bool\s+entered\s*,\s*bool\s+successful\s*\)",
        )
        self.assertIsNotNone(observe)
        self.assertIn("if (successful && !nativeTerminalKillChecked)", observe)
        self.assertIn("const bool dead = entity.IsDead();", observe)
        self.assertIn("const bool deadMode = entity.Routine.AIMode == gEAIMode_Dead &&", observe)
        self.assertIn("static_cast<int>(entity.Routine.AIMode) == 9;", observe)
        self.assertIn("nativeTerminalKills = 1;", observe)
        self.assertEqual(observe.count("nativeTerminalKills = 1"), 1)
        self.assertIn("native Kill confirmed from ZS_RagDollDead", observe)
        self.assertIn("ANOMALY native Kill not confirmed", observe)
        self.assertIn("killer=null victim=hero", observe)
        self.assertIn("if (!life.BeginDeath())", observe)
        self.assertIn("life.FinishDeath(true)", observe)
        self.assertNotIn("ObserveNativeKill", self.downed_h)
        self.assertNotIn("ObserveNativeKill", self.downed_cpp)
        keys = function_body(ai, r"\bGEInt\s+GE_STDCALL\s+FilterPlayerKeys\s*\(")
        self.assertIn("if (downed::Held() || downed::Terminal()) return 0;", keys)
        self.assertIn("persist::NativeLoading()", self.downed_cpp)
        self.assertIn("if (persist::HeroPending())", tick)
        redirect = function_body(self.downed_cpp, r"\bbool\s+RedirectDeath\s*\(")
        self.assertIn("!persist::HeroPending()", redirect)

    def test_terminal_native_boundaries_are_guarded_and_observed(self):
        ai = _testbuild.src_text("ai.cpp")
        enter = function_body(ai, r"\bbool\s+EnterTerminalDeath\s*\(")
        self.assertIn("bTObjStack<gScriptRunTimeSingleState> stack;", enter)
        self.assertIn("gScriptRunTimeSingleState state;", enter)
        self.assertIn('state.m_strScriptName = "ZS_RagDollDead";', enter)
        self.assertIn("stack.Add(state)", enter)
        self.assertIn("guards::Acting localPlayer(hero, false)", enter)
        self.assertIn("Run(originalStates, stack, &routine->GetSPU())", enter)
        self.assertNotIn("ProcessScript()", enter)
        self.assertNotIn("CallScript", enter)
        player_routine = function_body(ai, r"\bGEBool\s+GE_STDCALL\s+FilterPlayerRoutine\s*\(")
        self.assertIn("downed::GuardState", player_routine)
        self.assertLess(player_routine.index("downed::GuardState"), player_routine.index("originalPlayerRoutine(spu)"))
        guard = function_body(self.downed_cpp, r"\bbool\s+GuardState\s*\(")
        self.assertIn("life.TerminalStateAllowed(state)", guard)
        self.assertIn('KeepTerminal(hero, "GuardState")', guard)
        for name, policy in (("SetMode", "TerminalModeAllowed"), ("SetHp", "TerminalHpAllowed"),
                             ("SetTask", "TerminalTaskAllowed"), ("SetState", "TerminalStateAllowed")):
            setter = function_body(self.downed_cpp, rf"\b(?:void|GEBool)\s+__fastcall\s+{name}\s*\(")
            self.assertIn("LocalTerminal(entity)", setter)
            self.assertIn(policy, setter)
            self.assertIn("_ReturnAddress()", setter)
            self.assertIn("ObserveTerminal", setter)
        samples = function_body(self.downed_cpp, r"\bvoid\s+ObserveTerminal\s*\(")
        self.assertIn("now - terminalAt > 5000", samples)
        self.assertIn("now - terminalSampleAt < 100", samples)
        self.assertIn("end-state player=", samples)
        self.assertIn("hp=%d ai=%d task=%s", samples)
        ending = _testbuild.src_text("ending.cpp")
        self.assertIn("if (!on || downed::Terminal()) return;", function_body(ending, r"\bvoid\s+Tick\s*\("))

    def test_records_preserve_downed_state_before_restoring_hp(self):
        hero = _testbuild.src_text("hero.cpp")
        save = function_body(hero, r"\bbool\s+SaveRecordImpl\s*\(")
        load = function_body(hero, r"\bbool\s+LoadRecordImpl\s*\(")
        self.assertIn('fprintf(file, "downed %d\\n"', save)
        self.assertIn('downed::Restore(savedDowned != 0, "hero-record")', load)
        self.assertLess(load.index("downed::Restore"), load.index("self.Inventory.HoldStacks"))
        self.assertIn("downed::Held()", load)

    def test_paid_revive_is_bound_to_sender_and_down_episode(self):
        self.assertIn("sequence != life.sequence", self.downed_cpp)
        self.assertIn("life.Revive(pendingReviveSequence)", self.downed_cpp)
        self.assertIn("hero.IsDown() || hero.IsDead()", self.downed_all)
        tick = function_body(self.downed_cpp, r"\bvoid\s+Tick\s*\(\s*\)")
        refused = function_body(tick, r"\bif\s*\(\s*revivedBy\s*&&\s*!Able\(remotes\[revivedBy\],\s*now\)\s*\)")
        self.assertIsNotNone(refused)
        self.assertIn("revivedBy = 0;", refused)
        self.assertIn("pendingReviveSequence = 0;", refused)
        self.assertLess(tick.index("revivedBy && !Able(remotes[revivedBy], now)"),
                        tick.index("life.Revive(pendingReviveSequence)"))
        self.assertIn("life.terminal || !life.Revive(pendingReviveSequence)", tick)
        net = _testbuild.src_text("net.cpp")
        self.assertIn("message.state.npcClass = from;", net)
        self.assertIn("constexpr uint16_t ProtocolVersion = saving::Protocol;", net)

    def test_client_revive_world_states_and_kind_19_routing(self):
        net = _testbuild.src_text("net.cpp")
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
        gate = "if (!host || (!isHost && state.kind != 8 && state.kind != 9 && state.kind != 10)) return;"
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
        infocmd = _testbuild.src_text("infocmd.cpp")
        self.assertIn("if (net::IsHost() && s.kind != 3 && s.kind != 16) net::SendWorldState(s, from);", infocmd)
        apply = function_body(infocmd, r"\bvoid\s+Apply\s*\(")
        help_message = function_body(apply, r"\bif\s*\(\s*s\.kind\s*==\s*8\s*\|\|\s*s\.kind\s*==\s*9\s*\)")
        target = function_body(help_message, r"\bif\s*\(\s*s\.guard\s*==\s*net::LocalId\(\)\s*\)")
        self.assertIn("std::memcpy(&sequence, s.guid, sizeof(sequence));", target)
        self.assertIn("if (s.kind == 8) downed::Revived(s.npcClass, sequence);", target)
        self.assertIn("else downed::HelperHasNoPotion(s.npcClass);", target)

    def test_cached_end_step_reaches_crash_handlers(self):
        crash = _testbuild.src_text("crashlog.cpp")
        crash_h = _testbuild.src_text("crashlog.h")
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
        crash = _testbuild.src_text("crashlog.cpp")
        dump = function_body(crash, r"\bbool\s+WriteDump\s*\(")
        self.assertIsNotNone(dump)
        for flag in ("MiniDumpWithThreadInfo", "MiniDumpWithHandleData", "MiniDumpWithUnloadedModules",
                     "MiniDumpWithDataSegs", "MiniDumpWithIndirectlyReferencedMemory"):
            self.assertIn(flag, dump)
        self.assertEqual(dump.count("writeDump("), 1)
        self.assertIn("MiniDumpWithFullMemoryInfo", dump)
        self.assertNotIn("#ifdef MiniDumpWithFullMemoryInfo", dump)
        self.assertIn("const Profile profiles[]", dump)
        self.assertIn('"detailed"', dump)
        self.assertIn('"legacy-rich"', dump)
        self.assertIn('"normal"', dump)
        legacy = dump[dump.index("const auto legacyRich"):dump.index("struct Profile")]
        self.assertIn("MiniDumpWithHandleData", legacy)
        self.assertIn("MiniDumpWithUnloadedModules", legacy)
        self.assertIn("MiniDumpWithDataSegs", legacy)
        self.assertIn("MiniDumpWithIndirectlyReferencedMemory", legacy)
        self.assertNotIn("MiniDumpWithThreadInfo", legacy)
        self.assertNotIn("MiniDumpWithFullMemoryInfo", legacy)
        self.assertIn("rewind()", dump)
        self.assertIn("if (i && !rewind())", dump)
        self.assertIn("ERROR_INVALID_PARAMETER", dump)
        self.assertIn("static_cast<DWORD>(E_INVALIDARG)", dump)
        self.assertNotIn("== static_cast<DWORD>(E_FAIL)", dump)
        self.assertIn("crash dump success profile=%s", dump)
        self.assertIn("crash dump write failed profile=%s", dump)
        self.assertIn("crash dump write failed", dump)
        self.assertIn("WriteDump(file, dumpExceptionCopied ? &exception : nullptr);", crash)
        self.assertIn("WriteDump(file, nullptr);", crash)

    def test_dump_worker_owns_exception_context_and_busy_admission(self):
        crash = _testbuild.src_text("crashlog.cpp")
        self.assertIn("EXCEPTION_RECORD dumpExceptionRecord{};", crash)
        self.assertIn("CONTEXT dumpContext{};", crash)
        self.assertIn("EXCEPTION_POINTERS dumpExceptionPointers{};", crash)
        self.assertIn("bool dumpExceptionCopied = false;", crash)
        self.assertIn("LONG dumpBusy = 0;", crash)

        admit = function_body(crash, r"\bvoid\s+Dump\s*\(")
        self.assertIsNotNone(admit)
        self.assertIn("InterlockedCompareExchange(&dumpBusy, 1, 0)", admit)
        self.assertIn("guard: crashlog dump busy", admit)
        self.assertIn("ResetEvent(dumpDone)", admit)
        self.assertIn("dumpExceptionRecord = *info->ExceptionRecord;", admit)
        self.assertIn("dumpExceptionRecord.ExceptionRecord = nullptr;", admit)
        self.assertIn("dumpContext = *info->ContextRecord;", admit)
        self.assertIn("dumpExceptionPointers.ExceptionRecord = &dumpExceptionRecord;", admit)
        self.assertIn("dumpExceptionPointers.ContextRecord = &dumpContext;", admit)
        self.assertIn("dumpExceptionCopied = true;", admit)
        self.assertLess(admit.index("dumpExceptionRecord ="), admit.index("SetEvent(dumpRequest)"))
        self.assertIn("if (!SetEvent(dumpRequest))", admit)
        self.assertIn("const DWORD waited = WaitForSingleObject(dumpDone, wait);", admit)
        self.assertIn("WAIT_TIMEOUT", admit)
        self.assertIn("crash dump wait timeout", admit)
        self.assertIn("WAIT_FAILED", admit)
        self.assertIn("crash dump wait failed", admit)

        worker = function_body(crash, r"\bDWORD\s+WINAPI\s+DumpThread\s*\(")
        self.assertIsNotNone(worker)
        self.assertIn("dumpExceptionCopied ? &dumpExceptionPointers : nullptr", worker)
        self.assertIn("dumpExceptionCopied ? &exception : nullptr", worker)
        self.assertLess(worker.index("SetEvent(dumpDone);"),
                        worker.index("InterlockedExchange(&dumpBusy, 0);"))
        self.assertIn("InterlockedExchange(&dumpBusy, 0);", worker)
        handler = function_body(crash, r"\bLONG\s+CALLBACK\s+OnException\s*\(")
        self.assertIn("!info || !info->ExceptionRecord || !info->ContextRecord", handler)
        self.assertIn("guard: crashlog missing exception record/context", handler)

    def test_known_crash_symbols_use_actual_enet_entries_and_executable_rva_bounds(self):
        crash = _testbuild.src_text("crashlog.cpp")
        self.assertLess(crash.index("#include <enet/enet.h>"), crash.index("#include <Game.h>"))
        known = function_body(crash, r"\bvoid\s+FindKnownSymbols\s*\(")
        self.assertIsNotNone(known)
        for symbol in (
            "enet_range_coder_compress",
            "enet_range_coder_decompress",
            "enet_host_flush",
            "enet_host_service",
        ):
            self.assertIn(symbol, known)
            self.assertIn("&" + symbol, known)
        lookup = function_body(crash, r"\bchar\s+const\*\s+NearestKnownSymbol\s*\(")
        self.assertIsNotNone(lookup)
        self.assertLess(crash.index("bool Executable(Module const& m, uintptr_t rva);"),
                        crash.index("char const* NearestKnownSymbol"))
        self.assertIn("Executable(m, rva)", lookup)
        self.assertIn("Executable(m, at)", lookup)
        self.assertIn("at <= rva", lookup)
        self.assertIn("at > *start", lookup)
        self.assertIn("m.base + m.size", lookup)
        where = function_body(crash, r"\bbool\s+Where\s*\(")
        self.assertIsNotNone(where)
        self.assertNotIn("if (!name && m.game)", where)
        self.assertIn("NearestKnownSymbol(m, rva, &start)", where)
        self.assertIn("NearestExport(m, rva, &exportStart)", where)
        self.assertIn("exportStart > start", where)
        self.assertIn("scriptStart > start", where)
        self.assertIn("nearest known symbol", where)
        self.assertIn("nearest export symbol", where)
        self.assertIn("Executable(m, rva)", where)
        executable = crash.split("bool Executable(Module const& m, uintptr_t rva)\n", 1)[1].split(
            "// The exported function", 1)[0]
        self.assertIn("RvaRange(m, rva, 1)", executable)
        self.assertIn("Headers(m)", executable)
        export_lookup = function_body(crash, r"\bchar\s+const\*\s+NearestExport\s*\(")
        self.assertIsNotNone(export_lookup)
        self.assertIn("Headers(m)", export_lookup)
        self.assertIn("RvaRange(m, nameRvas[i], 1)", export_lookup)
        self.assertIn("NumberOfFunctions", export_lookup)
        self.assertIn("NumberOfNames", export_lookup)

    def test_engine_free_transition_and_ordering_rules(self):
        result = TARGET.run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_refused_stand_defers_repair_without_replaying_entry(self):
        guard = function_body(self.downed_cpp, r"\bbool\s+GuardState\s*\(")
        self.assertNotIn("FullStop", guard)
        self.assertNotIn(".SetTask(", guard)
        self.assertIn("if (!StableHeld(hero)) convergence.Request();", guard)
        self.assertIn("hero.Routine.SetTaskTime(0.0f);", guard)
        self.assertIn("hero.Routine.AIMode = gEAIMode_Down;", guard)

    def test_held_repair_rebuilds_loop_once_without_knockdown_entry(self):
        keep = function_body(self.downed_cpp, r"\bvoid\s+KeepDown\s*\(")
        self.assertIn("convergence.Observe(StableHeld(hero)", keep)
        self.assertIn("body::MotionFraction(hero.GetInstance())", keep)
        repair = function_body(keep, r"\bif\s*\(\s*repair\s*\)")
        self.assertIn("hero.StopPlayAni();", repair)
        self.assertIn("routine->AIStopPlayAnimation();", repair)
        self.assertIn("routine->SetCurrentTask(bCString(KnockOutTask));", repair)
        self.assertIn('routine->AISetState(bCString("ZS_Unconscious_Loop"));', repair)
        self.assertIn("hero.Routine.AniState = gEAniState_LieKnockOut;", repair)
        self.assertNotIn(".SetTask(", keep)
        self.assertNotIn("AISetTask", keep)
        self.assertLess(keep.index("convergence.Observe"), keep.index("hero.Routine.FullStop()"))

    def test_stable_held_requires_loop_pose_and_no_rising_motion(self):
        stable = function_body(self.downed_cpp, r"\bbool\s+StableHeld\s*\(")
        for value in ("ZS_Unconscious_Loop", "gEAniState_LieKnockOut", "GetUp", "StandUp", "KnockDown", "_Ambient_"):
            self.assertIn(value, stable)

    def test_native_setters_refuse_wake_before_replacing_held_stack(self):
        for function, native in (("SetTask", "originalSetTask"), ("SetState", "originalSetState")):
            setter = function_body(self.downed_cpp, rf"\bvoid\s+__fastcall\s+{function}\s*\(")
            self.assertIn("RejectHeldTransition", setter)
            self.assertLess(setter.index("RejectHeldTransition"), setter.index(native))
        guard = function_body(self.downed_cpp, r"\bbool\s+RejectHeldTransition\s*\(")
        for condition in ("!life.held", "life.terminal", "GetHero()", "persist::NativeLoading()",
                          "persist::HeroPending()", "ai::ShuttingDown()", "DownTask(state)", "ZS_Unconscious_Loop"):
            self.assertIn(condition, guard)
        for mutation in ("FullStop", "SetTask(", "AISetState(", "ContinueRoutine"):
            self.assertNotIn(mutation, guard)

    def test_convergence_reset_and_resume_are_local_life_only(self):
        for signature in (r"\bvoid\s+Hold\s*\(", r"\bvoid\s+OnWorldChange\s*\("):
            self.assertIn("convergence.Clear();", function_body(self.downed_cpp, signature))
        tick = function_body(self.downed_cpp, r"\bvoid\s+Tick\s*\(")
        self.assertLess(tick.index("life.Revive("), tick.index("convergence.Clear();"))
        resume = function_body(self.downed_cpp, r"\bvoid\s+OnFrameResume\s*\(")
        self.assertIn("life.held && !life.terminal", resume)
        self.assertIn("convergence.Request();", resume)
        for mutation in ("FullStop", "SetTask", "SetHitPoints", "ContinueRoutine"):
            self.assertNotIn(mutation, resume)

    def test_focus_observation_does_not_mutate_npcs_or_input(self):
        observe = function_body(self.script_cpp, r"\bvoid\s+ObserveFocus\s*\(")
        for field in ("focus: %s", "gap_ms=%u", "away_ms=%u", "native_dt_ms=%.1f", "task=%.48s", "state=%.48s",
                      "weapon_changed=%zu", "ai_changed=%zu", "draw_motion=%zu", "actor_replacements=%u"):
            self.assertIn(field, observe)
        self.assertIn("g3t::partner::Focused()", observe)
        self.assertIn("g3t::downed::OnFrameResume();", observe)
        self.assertIn("g3t::guid::FormatEntity", observe)
        self.assertIn("ResetFocusObservation();", function_body(self.script_cpp, r"\bstatic\s+void\s+ForgetWorld\s*\("))
        for mutation in ("FullStop", "SetTask(", "SetAIMode", "EnableProcessing", "SetCombatMode", "SetForegroundWindow",
                         "SendInput", "SendWorldState", "SendNpc"):
            self.assertNotIn(mutation, observe)
        for callback, log in (("ResetResources", "resource-reset"), ("RestoreResources", "resource-restored")):
            body = function_body(self.script_cpp, rf"\bbEResult\s+{callback}\s*\(")
            self.assertIn("focus: " + log, body)
            self.assertNotIn("ForgetWorld", body)
            self.assertNotIn("FullStop", body)
        restore = function_body(self.script_cpp, r"\bbEResult\s+RestoreResources\s*\(")
        self.assertIn("g3t::downed::OnFrameResume();", restore)

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
        policy = _testbuild.src_text("downed_policy.h")
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
        net = _testbuild.src_text("net.cpp")
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
        net = _testbuild.src_text("net.cpp")
        clear = function_body(net, r"\bvoid\s+ClearPresentation\s*\(")
        self.assertIn("cleared.activity = remotes[id].activity", clear)
        self.assertIn("cleared.connected = remotes[id].connected", clear)
        gate = function_body(net, r"\bvoid\s+SetInWorld\s*\(")
        self.assertIn("ClearPresentation(i)", gate)
        self.assertIn("ClearPresentation(id)", gate)
        drop = function_body(net, r"\bvoid\s+Drop\s*\(")
        self.assertIn("remotes[id] = Remote{}", drop)

    def test_arriving_held_puppet_uses_owner_life_not_buffered_wake(self):
        remote = _testbuild.src_text("remote.cpp")
        update = function_body(remote, r"\bvoid\s+UpdatePuppet\s*\(")
        held = function_body(update, r"\bif\s*\(\s*held\s*\)")
        local_state = function_body(remote, r"\bbool\s+LocalState\s*\(")
        for requirement in ("target.position = remote.latest.position", "target.speed = 0.0f",
                            "target.movementMode = gECharMovementMode_1",
                            "target.aniState = gEAniState_LieKnockDown",
                            "world::RequestProcessingUpdate()", 'std::strstr(target.motion, "GetUp")'):
            self.assertIn(requirement, held)
        self.assertIn("Entity(own).IsDead() || downed::Terminal()", local_state)
        self.assertIn("dead && !puppet.terminal", update)
        self.assertIn("StartPuppetTerminal(puppet, id, hero, now)", update)
        terminal = function_body(update, r"\bif\s*\(\s*puppet\.terminal\s*\)")
        self.assertIsNotNone(terminal)
        self.assertIn("UpdatePuppetTerminal(puppet, id, hero, now)", terminal)
        self.assertIn("return;", terminal)
        self.assertNotIn("body::Drive", terminal)
        self.assertNotIn("AniState = gEAniState_LieKnockDown", terminal)
        self.assertIn("bCString(\"DEAD\")", remote)
        self.assertIn('puppet.entity.StartSaySVM(target, bCString("DEAD"), GEFalse);', remote)
        self.assertNotIn('puppet.entity.StartSaySVM(target, bCString("DEAD"), GETrue);', remote)
        self.assertIn("npcplayback::CapturePose", remote)
        self.assertIn("npcplayback::ReplayFrozenPose", remote)
        self.assertIn("SetRagDollEnabled(GETrue, 0.0f, GETrue)", remote)
        self.assertIn("gECharMovementMode_RagDollDead", remote)
        self.assertNotIn("AddForce", remote[remote.index("void StartPuppetTerminal"):remote.index("void UpdatePuppet", remote.index("void StartPuppetTerminal"))])
        self.assertIn("puppet.entity.Routine.AniState = gEAniState_LieKnockDown", update)
        self.assertIn("|| heldOwner", update)
        self.assertIn("remote.activity.downedSequence == remote.latest.downedSequence", update)
        self.assertIn("puppet.entity.Dialog.TradeEnabled", update)
        self.assertNotIn('CallScript(bCString("Kill")', update)
        pay = function_body(self.downed_cpp, r"\bvoid\s+PayAndHelp\s*\(")
        self.assertIn("!peers[i].payment.NeedsPayment(r.latest.downedSequence)", pay)

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
