import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_downed_policy import function_body
from test_hud_notices import read_rva


ROOT = Path(__file__).resolve().parents[1]


class ClientMovementTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.guards = (ROOT / "src" / "guards.cpp").read_text(encoding="utf-8")
        cls.ai = (ROOT / "src" / "ai.cpp").read_text(encoding="utf-8")
        cls.movement = (ROOT / "src" / "movement.cpp").read_text(encoding="utf-8")
        cls.module = (ROOT / "src" / "Script_G3Together.cpp").read_text(encoding="utf-8")
        cls.downed = (ROOT / "src" / "downed.cpp").read_text(encoding="utf-8")

    def test_engine_free_nested_identity_and_stop_thresholds(self):
        # Compile the actual scope method bodies with inert entities, not a reimplementation.
        acting = function_body(self.guards, r"Acting::Acting\s*\(")
        acting_end = function_body(self.guards, r"Acting::~Acting\s*\(")
        local = function_body(self.guards, r"LocalPlayer::LocalPlayer\s*\(")
        local_end = function_body(self.guards, r"LocalPlayer::~LocalPlayer\s*\(")
        self.assertIn("LocalPlayer::LocalPlayer(bool local) : before(actor)", self.guards)
        self.assertIn("Acting::Acting(eCEntity* puppet, bool keepChapter) : before(actor)", self.guards)
        harness = f"""
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
        compiler = shutil.which(os.environ.get("CXX", "clang++")) or shutil.which("g++") or shutil.which("cl")
        self.assertIsNotNone(compiler, "Engine-free movement regression needs a C++ compiler")
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source = directory / "scope.cpp"
            source.write_text(harness, encoding="utf-8")
            exe = directory / ("movement.exe" if os.name == "nt" else "movement")
            policy = ROOT / "tests" / "client_movement_test.cpp"
            if Path(compiler).stem.lower() == "cl":
                command = [compiler, "/nologo", "/std:c++17", "/EHsc", str(source), str(policy),
                           f"/I{ROOT / 'src'}", f"/Fe:{exe}"]
            else:
                command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(source), str(policy),
                           "-I", str(ROOT / "src"), "-o", str(exe)]
            compiled = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            executed = subprocess.run([str(exe)], cwd=directory, capture_output=True, text=True, timeout=20)
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
        logger = (ROOT / "src" / "log.cpp").read_text(encoding="utf-8")
        self.assertIn("char text[16384]", logger)
        self.assertIn("char line[16448]", logger)
        reset = function_body(self.movement, r"\bvoid\s+OnWorldChange\s*\(")
        for clear in ("previous = {}", "boundary = {}", "calls = {}", "stop.Reset()"):
            self.assertIn(clear, reset)

    def test_processing_refresh_never_clears_live_membership(self):
        world = (ROOT / "src" / "world.cpp").read_text(encoding="utf-8")
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
        remote = (ROOT / "src" / "remote.cpp").read_text(encoding="utf-8")
        for name in ("remote capture", "ENet service", "remote puppet update"):
            self.assertIn(f'Stage stage("{name}")', remote)
        net = (ROOT / "src" / "net.cpp").read_text(encoding="utf-8")
        self.assertIn('Stage stage("ENet receive")', net)

    def test_read_only_engine_bytes_match_dirty_write_and_exit_observer(self):
        game = os.environ.get("G3T_GAME_DLL")
        if not game:
            self.skipTest("Set G3T_GAME_DLL for sibling Engine.dll native signatures")
        data = Path(game).with_name("Engine.dll").read_bytes()
        self.assertEqual(read_rva(data, 0x231e00, 17),
                         bytes.fromhex("56 8b f1 e8 f8 fb ff ff c6 86 fd 00 00 00 01 5e c3"))
        self.assertEqual(read_rva(data, 0x223af0, 6), bytes.fromhex("56 8b f1 8b 46 3c"))
        self.assertIn("exitEntry == engine + 0x223af0", self.movement)

    def test_frame_order_and_complete_tick_stage_coverage(self):
        process = function_body(self.module, r"\bvoid\s+Process\s*\(\s*\)\s+override")
        self.assertLess(process.index("g3t::remote::Tick()"), process.index("g3t::ai::Tick()"))
        self.assertLess(process.index("g3t::ai::Tick()"), process.index("g3t::npcs::Tick()"))
        for name in ("NPC / lease", "NPC playback", "remote / ENet service", "combat", "downed", "world", "persist"):
            self.assertIn(f'Stage stage("{name}")', process)
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
        data = Path(path).read_bytes()
        self.assertEqual(read_rva(data, 0xEB660, 6), bytes.fromhex("55 8b ec 83 e4 f8"))
        self.assertEqual(read_rva(data, 0x164430, 6), bytes.fromhex("8d 41 64 8b 48 74"))
        self.assertIn("stopEntry == game + 0xeb660", self.movement)
        self.assertIn("fullEntry == game + 0x164430", self.movement)
