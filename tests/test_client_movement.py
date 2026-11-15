import os
from pathlib import Path
import re
import shutil
import sys
import unittest

from test_downed_policy import function_body
from test_hud_notices import read_rva


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

_guards = _testbuild.src_text("guards.cpp")
acting = function_body(_guards, r"Acting::Acting\s*\(")
acting_end = function_body(_guards, r"Acting::~Acting\s*\(")
local = function_body(_guards, r"LocalPlayer::LocalPlayer\s*\(")
local_end = function_body(_guards, r"LocalPlayer::~LocalPlayer\s*\(")
HARNESS = f"""
#include "guards.h"
#include "movement.h"
#include <cassert>
class eCEntity {{}};
static eCEntity hero;
static unsigned observations = 0, chapters = 0;
namespace g3t::remote {{ eCEntity* ControlledBody() {{ return &hero; }} }}
namespace g3t::movement {{
void Touch(eCEntity*, char const*, char const*, void*) {{ ++observations; }}
}}
namespace g3t::guards {{
static thread_local eCEntity* actor = nullptr;
void KeepChapter(eCEntity*) {{ ++chapters; }}
Acting::Acting(eCEntity* puppet, bool keepChapter) : before(actor) {{{acting}}}
Acting::~Acting() {{{acting_end}}}
LocalPlayer::LocalPlayer(bool local) : before(actor) {{{local}}}
LocalPlayer::~LocalPlayer() {{{local_end}}}
eCEntity* CurrentActor() {{ return actor; }}
}}
void TestMovementPolicy();
int main() {{
    using namespace g3t::guards;
    eCEntity a, b;
    assert(CurrentActor() == nullptr);
    {{
        Acting npc(&a, false);
        {{ Acting inherited(nullptr); assert(CurrentActor() == &a); }}
        {{ LocalPlayer notLocal(false); assert(CurrentActor() == &a); }}
        {{
            LocalPlayer player;
            assert(CurrentActor() == nullptr);
            {{
                Acting nestedNpc(&b, false);
                {{ LocalPlayer nestedPlayer; assert(CurrentActor() == nullptr); }}
                assert(CurrentActor() == &b);
            }}
            assert(CurrentActor() == nullptr);
        }}
        assert(CurrentActor() == &a);
        try {{ LocalPlayer player; throw 1; }} catch (int) {{}}
        assert(CurrentActor() == &a);
    }}
    assert(CurrentActor() == nullptr);
    assert(observations == 3 && chapters == 0);
    TestMovementPolicy();
}}
"""
_compiler = shutil.which(os.environ.get("CXX", "clang++")) or shutil.which("g++") or shutil.which("cl")
_warnings = "none" if _compiler and Path(_compiler).stem.lower() == "cl" else "strict"
TARGET = _testbuild.register_target(
    "client_movement",
    [_testbuild.inline_unit("scope.cpp", HARNESS, warnings=_warnings, includes=(ROOT / "src",)),
     _testbuild.unit(ROOT / "tests" / "client_movement_test.cpp",
                     warnings=_warnings, includes=(ROOT / "src",))],
    msvc_vcvars=False,
)


class ClientMovementTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.guards = _testbuild.src_text("guards.cpp")
        cls.ai = _testbuild.src_text("ai.cpp")
        cls.movement = _testbuild.src_text("movement.cpp")
        cls.module = _testbuild.src_text("Script_G3Together.cpp")
        cls.downed = _testbuild.src_text("downed.cpp")

    def test_engine_free_nested_identity_and_stop_thresholds(self):
        self.assertIn("LocalPlayer::LocalPlayer(bool local) : before(actor)", self.guards)
        self.assertIn("Acting::Acting(eCEntity* puppet, bool keepChapter) : before(actor)", self.guards)
        executed = TARGET.run(timeout=20)
        self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

    def test_all_local_script_boundaries_isolate_identity_before_native_work(self):
        for signature, native in (
            (r"\bGEBool\s+Run\s*\(", "downed::GuardState"),
            (r"\bGEInt\s+GE_STDCALL\s+FilterContinue\s*\(", "downed::GuardState"),
            (r"\bGEInt\s+GE_STDCALL\s+FilterPlayerKeys\s*\(", "originalPlayerKeys(spu"),
            (r"\bGEBool\s+GE_STDCALL\s+FilterPlayerRoutine\s*\(", "originalPlayerRoutine(spu"),
        ):
            body = function_body(self.ai, signature)
            self.assertIsNotNone(body, signature)
            self.assertLess(body.index("guards::LocalPlayer"), body.index(native))
        keys = function_body(self.ai, r"\bGEInt\s+GE_STDCALL\s+FilterPlayerKeys\s*\(")
        self.assertIn("if (downed::Held() || downed::Terminal()) return 0", keys)
        self.assertIn("movement::KeyDispatch", keys)

    def test_diagnostics_never_replay_keys_or_repair_hero(self):
        for mutation in ("SendInput", "keybd_event", "SetToWorldMatrix(", "SetPose(", ".MoveTo(",
                         ".StopMovement(", ".FullStop(", ".SetTask(", ".SetState(", "EnableInput("):
            self.assertNotIn(mutation, self.movement)
        for signature, original in ((r"\bvoid\s+__fastcall\s+NativeStop\s*\(", "originalStop(self, edx)"),
                                    (r"\bvoid\s+__fastcall\s+NativeFullStop\s*\(", "originalFullStop(self, edx)"),
                                    (r"\bvoid\s+__fastcall\s+NativeExit\s*\(", "originalExit(self, edx)")):
            body = function_body(self.movement, signature)
            self.assertIn(original, body)
            self.assertNotIn("return", body)
            self.assertNotIn("Log", body)
        touch = function_body(self.movement, r"\bvoid\s+Touch\s*\(")
        self.assertIn("!guards::IsLocalPlayer(entity)", touch)
        self.assertIn("persist::NativeLoading()", touch)
        for io in ("Logf", "Log(", "WriteFile", "CreateFile", "fopen", "new "):
            self.assertNotIn(io, touch)
        capture = function_body(self.movement, r"\bSample\s+Capture\s*\(")
        self.assertLess(capture.index("persist::NativeLoading()"), capture.index("remote::ControlledBody()"))
        self.assertIn("context_clears=%u", self.movement)

    def test_fixed_storage_and_bounded_log_trail(self):
        self.assertIn("std::array<Call, 16>", self.movement)
        trail = function_body(self.movement, r"\bvoid\s+Trail\s*\(")
        self.assertIn("const unsigned count = callCount", trail)
        self.assertIn("now - call.at > 2000000", trail)
        self.assertNotIn("Logf", trail)
        self.assertIn("stage_call=%u stack=%s", trail)
        event = function_body(self.movement, r"\bvoid\s+Event\s*\(")
        self.assertIn('std::strcmp(kind, "held-key-stop")', event)
        self.assertIn("now - anomalyAt < 1000000", event)
        self.assertIn("trail=%s", event)
        self.assertIn("frame_native_exit=%u", event)
        self.assertIn("(current.input & 7u) == 7u", self.movement)
        self.assertEqual(event.count("Logf("), 1)
        logger = _testbuild.src_text("log.cpp")
        self.assertIn("char text[16384]", logger)
        self.assertIn("char line[16448]", logger)
        reset = function_body(self.movement, r"\bvoid\s+OnWorldChange\s*\(")
        for clear in ("previous = {}", "boundary = {}", "calls = {}", "stop.Reset()"):
            self.assertIn(clear, reset)

    def test_processing_refresh_never_clears_live_membership(self):
        world = _testbuild.src_text("world.cpp")
        request = function_body(world, r"\bvoid\s+RequestProcessingUpdate\s*\(")
        self.assertIn("MarkProcessingDirty", request)
        self.assertIn('"ROI-dirty"', request)
        self.assertNotIn("FourceFullROIUpdate(", world)
        mark = function_body(world, r"\bbool\s+MarkProcessingDirty\s*\(")
        self.assertIn("QueueProcessingRebuild", mark)
        self.assertIn("0x231e00", mark)
        self.assertIn("destructive full ROI clear refused", mark)
        place = function_body(world, r"\bvoid\s+PlaceSpheres\s*\(")
        self.assertIn("if (!MarkProcessingDirty(admin)) return", place)

    def test_native_counters_are_main_thread_and_substage_attributed(self):
        touch = function_body(self.movement, r"\bvoid\s+Touch\s*\(")
        self.assertLess(touch.index("GetCurrentThreadId() != observerThread"),
                        touch.index("guards::IsLocalPlayer(entity)"))
        before = function_body(self.movement, r"\bSample\s+BeforeTransform\s*\(")
        self.assertLess(before.index("GetCurrentThreadId() != observerThread"),
                        before.index("guards::IsLocalPlayer(entity)"))
        after = function_body(self.movement, r"\bvoid\s+AfterTransform\s*\(")
        self.assertIn("after.capturedAt - before.capturedAt", after)
        self.assertNotIn("previousSampleAt", after)
        self.assertIn("++nativeStops", touch)
        self.assertIn("++nativeFullStops", touch)
        self.assertIn("++nativeExits", touch)
        self.assertIn("last.stage == phase", touch)
        remote = _testbuild.src_text("remote.cpp")
        for name in ("remote capture", "ENet service", "remote puppet update"):
            self.assertIn(f'Stage stage("{name}")', remote)
        net = _testbuild.src_text("net.cpp")
        self.assertIn('Stage stage("ENet receive")', net)

    def test_read_only_engine_bytes_match_dirty_write_and_exit_observer(self):
        game = os.environ.get("G3T_GAME_DLL")
        if not game:
            self.skipTest("Set G3T_GAME_DLL for sibling Engine.dll native signatures")
        data = _testbuild.cached_bytes(Path(game).with_name("Engine.dll"))
        self.assertEqual(read_rva(data, 0x231e00, 17),
                         bytes.fromhex("56 8b f1 e8 f8 fb ff ff c6 86 fd 00 00 00 01 5e c3"))
        self.assertEqual(read_rva(data, 0x223af0, 6), bytes.fromhex("56 8b f1 8b 46 3c"))
        self.assertIn("exitEntry == engine + 0x223af0", self.movement)

    def test_frame_order_and_complete_tick_stage_coverage(self):
        process = function_body(self.module, r"\bvoid\s+Process\s*\(\s*\)\s+override")
        self.assertLess(process.index("&g3t::remote::Tick"), process.index("&g3t::ai::Tick"))
        self.assertLess(process.index("&g3t::ai::Tick"), process.index("&g3t::npcs::Tick"))
        for name in ("NPC / lease", "NPC playback", "remote / ENet service", "combat", "downed", "world", "persist"):
            self.assertRegex(process, rf'(?:Stage\s+stage|(?:Timed)?Step)\s*\(\s*"{re.escape(name)}"')
        frame = function_body(self.module, r"~FrameScope\s*\(")
        self.assertLess(frame.index("g3t::net::Flush()"), frame.index("g3t::persist::EndFrame()"))
        self.assertLess(frame.index("g3t::hud::Tick()"), frame.index("g3t::movement::End()"))
        self.assertIn("moduleProcessDepth == 1", self.module)
        self.assertIn("if (moduleProcessDepth == 0) g3t::movement::End()", frame)

    def test_existing_task_and_mode_hooks_record_before_any_rejection(self):
        for signature, record, gate in (
            (r"\bvoid\s+__fastcall\s+SetTask\s*\(", '"AISetTask"', "RejectReaction"),
            (r"\bvoid\s+__fastcall\s+SetState\s*\(", '"AISetState"', "RejectReaction"),
            (r"\bvoid\s+__fastcall\s+SetMode\s*\(", '"SetAIMode"', "LocalTerminal"),
        ):
            body = function_body(self.downed, signature)
            self.assertLess(body.index(record), body.index(gate))

    def test_observer_prologues_match_installed_read_only_game_bytes(self):
        path = os.environ.get("G3T_GAME_DLL")
        if not path:
            self.skipTest("Set G3T_GAME_DLL for read-only CP 1.75 native signatures")
        data = _testbuild.cached_bytes(path)
        self.assertEqual(read_rva(data, 0xEB660, 6), bytes.fromhex("55 8b ec 83 e4 f8"))
        self.assertEqual(read_rva(data, 0x164430, 6), bytes.fromhex("8d 41 64 8b 48 74"))
        self.assertIn("stopEntry == game + 0xeb660", self.movement)
        self.assertIn("fullEntry == game + 0x164430", self.movement)
