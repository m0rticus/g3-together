import os
import re
import struct
import sys
import unittest
from pathlib import Path

from test_downed_policy import function_body


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild


HUD_ENTITIES_POLICY_CODE = r"""
#undef NDEBUG
#include <cassert>
#include <cstring>
#include <vector>
#include "hud_entities.h"

int main()
{
    using namespace g3t::hud::entities;

    int a = 1, b = 2;
    assert(!ShouldScrubSlotOnDestroy(&a, nullptr));
    assert(!ShouldScrubSlotOnDestroy(nullptr, &a));
    assert(!ShouldScrubSlotOnDestroy(&a, &b));
    assert(ShouldScrubSlotOnDestroy(&a, &a));

    assert(!ShouldScrubIdleSlot(PlayerSlot, -1));
    assert(!ShouldScrubIdleSlot(FocusSlot, -1));
    assert(!ShouldScrubIdleSlot(FocusSlot, 0));
    assert(!ShouldScrubIdleSlot(FirstTransientPageSlot, 0));
    for (int slot = FirstTransientPageSlot; slot < EntitySlotCount; ++slot)
        assert(ShouldScrubIdleSlot(slot, -1));
    assert(!ShouldScrubIdleSlot(EntitySlotCount, -1));

    assert(!ValidActivePage(-1));
    assert(ValidActivePage(0));
    assert(ValidActivePage(PageSlotCount - 1));
    assert(!ValidActivePage(PageSlotCount));

    std::vector<std::uint8_t> image(SetActivePageRva + sizeof(SetActivePagePrefixBytes) + 16, 0);
    const auto base = reinterpret_cast<uintptr_t>(image.data());
    assert(!VerifyHudEntityLayout(nullptr, base + ClearAllEntitiesWrapperRva));
    assert(!VerifyHudEntityLayout(image.data(), base + ClearAllEntitiesWrapperRva + 4));
    assert(!VerifyHudEntityLayout(image.data(), base + ClearAllEntitiesWrapperRva));

    std::memcpy(image.data() + ClearAllEntitiesWrapperRva, ClearAllEntitiesWrapperBytes, sizeof(ClearAllEntitiesWrapperBytes));
    std::memcpy(image.data() + DialogClearAllEntitiesRva, DialogClearAllEntitiesBytes, sizeof(DialogClearAllEntitiesBytes));
    std::memcpy(image.data() + SetActivePageRva, SetActivePagePrefixBytes, sizeof(SetActivePagePrefixBytes));
    assert(VerifyHudEntityLayout(image.data(), base + ClearAllEntitiesWrapperRva));

    image[SetActivePageRva + 7] ^= 0xff;
    assert(!VerifyHudEntityLayout(image.data(), base + ClearAllEntitiesWrapperRva));
    return 0;
}
"""

HUD_ENTITIES_TARGET = _testbuild.register_target(
    "hud_entities_policy",
    [_testbuild.inline_unit("hud_entities_policy_test.cpp", HUD_ENTITIES_POLICY_CODE)],
    msvc_vcvars=False,
)


def read_rva(data, rva, size):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    sections = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    table = pe + 24 + optional_size
    for index in range(sections):
        section = table + index * 40
        virtual_size, address, raw_size, raw_offset = struct.unpack_from("<IIII", data, section + 8)
        if address <= rva and rva + size <= address + min(virtual_size, raw_size):
            offset = raw_offset + rva - address
            return data[offset:offset + size]
    raise AssertionError(f"Unmapped native code RVA {rva:#x}")


class HudNoticeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.hud = _testbuild.src_text("hud.cpp")
        cls.header = _testbuild.src_text("hud.h")
        cls.module = _testbuild.src_text("Script_G3Together.cpp")

    def test_message_copies_text_only_without_engine_or_file_io(self):
        body = function_body(self.hud, r"\bvoid\s+Message\s*\(")
        self.assertIn("std::lock_guard<std::mutex> lock(noticeMutex)", body)
        self.assertIn("pending.push_back({kind, text})", body)
        self.assertIn("struct Notice { Kind kind; std::wstring text; }", self.hud)
        for call in ("gui2", "PrintGameMessage", "GetInstance", "GetHero", "Log", "Display",
                     "StartOutput", "FullStop", "StopMovement"):
            self.assertNotIn(call, body)

    def test_notices_use_only_passive_native_list_and_original_style(self):
        display = function_body(self.hud, r"\bbool\s+Display\s*\(")
        self.assertIn("AppendText(list, bCUnicodeString(notice.text.c_str()), color, 10, 29, 6.0f, &offset)", display)
        self.assertIn("const bCVector2 offset(0.0f, -20.0f)", display)
        for kind, value, color in (("Info", 0, "3f2aac"), ("Good", 8, "3f2a94"),
                                   ("Warning", 12, "3f2ab8"), ("Bad", 13, "3f2a88")):
            self.assertRegex(self.header, rf"\b{kind}\s*=\s*{value}\b")
            self.assertIn(f"case {kind}: colorRva = 0x{color}; break;", display)
        for call in ("GetHero", "GetPlayer", "FullStop", "StopMovement", "SetTask", "AISet",
                     "StartOutput", "StartSay", "SetFocus", "BringWindowToTop", "ShowSubtitle"):
            self.assertNotIn(call, display)
        for source in (ROOT / "src").glob("*.cpp"):
            text = _testbuild.src_text(source.relative_to(ROOT / "src").as_posix())
            self.assertNotRegex(text, r"(?:gui2\.|manager->)PrintGameMessage\s*\(", str(source))

    def test_unavailable_display_preserves_all_queued_notices_without_caps(self):
        tick = function_body(self.hud, r"\bvoid\s+Tick\s*\(")
        unavailable = function_body(tick, r"\bif\s*\(\s*!list\s*\)")
        self.assertIn("notices queued until the passive HUD is ready", unavailable)
        self.assertIn("return;", unavailable)
        self.assertNotIn("pending.clear", unavailable)
        self.assertNotIn("swap", unavailable)
        self.assertLess(tick.index("if (!list)"), tick.index("batch.swap(pending)"))
        self.assertIn("for (auto const& notice : batch)", tick)
        for policy in ("resize(", "pop_back(", "credit", "budget", "throttle", "lastMessage", "dedup"):
            self.assertNotIn(policy, tick)
        self.assertNotIn("pending.clear", self.hud)
        self.assertIn('Logf("hud: [%d] %s"', tick)

    def test_flush_is_outermost_module_boundary_after_gameplay_and_network(self):
        frame = function_body(self.module, r"~FrameScope\s*\(\s*\)")
        self.assertLess(frame.index("g3t::net::Flush()"), frame.index("g3t::persist::EndFrame()"))
        self.assertLess(frame.index("g3t::persist::EndFrame()"), frame.index("g3t::hud::Tick()"))
        self.assertIn("if (--moduleProcessDepth == 0) g3t::hud::Tick()", frame)
        self.assertIn("if (run) ++moduleProcessDepth", self.module)
        self.assertEqual(self.module.count("g3t::hud::Tick()"), 1)
        tick = function_body(self.hud, r"\bvoid\s+Tick\s*\(")
        guard = tick[:tick.index("SampleDue")]
        for condition in ("ai::ShuttingDown()", "persist::NativeLoading()", "!gCSession::GetInstance().IsGameRunning()"):
            self.assertIn(condition, guard)

    def test_diagnostics_are_read_only_and_do_not_keep_world_entity_pointers(self):
        capture = function_body(self.hud, r"\bSnapshot\s+Capture\s*\(")
        for mutator in ("FullStop", "StopMovement", "SetTask", "SetState", "EnableInput",
                        "SetFocus", "SetMovement", "StartSay", "CallScript"):
            self.assertNotIn(mutator, capture)
        for field in ("GetCurrentTask", "GetCurrentState", "GetLocalVelocity", "CanPlayerMove",
                      "IsInputEnabled", "GetActiveWnd", "GetFocusWnd", "GetMovementMode",
                      "IsMovementEnabledFromSPU", "GetWorldRotation", "downed::Held"):
            self.assertIn(field, capture)
        observation = function_body(self.hud, r"\bstruct\s+Observation\b")
        self.assertIn("uintptr_t body", observation)
        self.assertNotIn("*", observation)
        due = function_body(self.hud, r"\bvoid\s+SampleDue\s*\(")
        self.assertIn("elapsed < 100", due)
        self.assertIn("elapsed >= 500", due)
        self.assertIn("snapshot.body != it->body", due)
        for phase in ("phase=100ms", "phase=500ms", "phase=cancelled"):
            self.assertIn(phase, due)
        reset = function_body(self.hud, r"\bvoid\s+OnWorldChange\s*\(")
        self.assertIn("observations.clear()", reset)
        self.assertNotIn("pending", reset)
        self.assertIn('ForgetStep("HUD observation cleanup", &g3t::hud::OnWorldChange)', self.module)

    def test_partner_notice_loop_never_applies_local_down_guards(self):
        downed = _testbuild.src_text("downed.cpp")
        tick = function_body(downed, r"\bvoid\s+Tick\s*\(")
        notices = tick[tick.index("// Tell this player"):tick.index("if (life.terminal)")]
        self.assertIn("if (i == self) continue;", notices)
        self.assertIn("hud::Message(hud::Warning, text)", notices)
        for action in ("Hold(", "KeepDown(", "StandUp(", "FullStop", "StopMovement", "EnableInput"):
            self.assertNotIn(action, notices)
        guard = function_body(downed, r"\bbool\s+GuardState\s*\(")
        self.assertIn("(!life.held && !life.terminal)", guard)
        self.assertIn("self != gCSession::GetInstance().GetHero()", guard)

    def test_native_adjustor_preserves_argument_order_and_stdcall_stack(self):
        body = function_body(self.hud, r"\bvoid\s+__stdcall\s+AppendText\s*\(")
        assembly = function_body(body, r"\b__asm\b")
        instructions = [line.strip() for line in assembly.splitlines() if line.strip()]
        self.assertEqual(instructions, [
            "mov eax, [esp + 4]", "pop edx", "add esp, 4", "push edx", "jmp dword ptr [appendText]",
        ])
        stack = ["return", "list", "text", "color", "font", "alignment", "lifetime", "offset"]
        registers = {}
        for instruction in instructions:
            if instruction == "mov eax, [esp + 4]":
                registers["eax"] = stack[1]
            elif instruction == "pop edx":
                registers["edx"] = stack.pop(0)
            elif instruction == "add esp, 4":
                stack.pop(0)
            elif instruction == "push edx":
                stack.insert(0, registers["edx"])
        self.assertEqual(registers["eax"], "list")
        self.assertEqual(stack, ["return", "text", "color", "font", "alignment", "lifetime", "offset"])
        # The native return consumes its return address and six arguments, including the extra slot removed above.
        self.assertEqual(len(stack) * 4 + 4, 8 * 4)

    def test_native_binding_refuses_unsupported_layout_before_publishing_pointer(self):
        bind = function_body(self.hud, r"\bbool\s+BindPresentation\s*\(")
        for contract in ("RootLayout", "ListLayout", "AppendArguments", "AppendEntry", "AppendReturn"):
            self.assertIn(f"sizeof({contract})", bind)
        self.assertIn("ABI mismatch; notices retained, no activating fallback", bind)
        self.assertLess(bind.index("std::memcmp"), bind.index("appendText = bytes + AppendRva"))

    def test_configured_game_matches_passive_append_abi_and_native_palette(self):
        path = os.environ.get("G3T_GAME_DLL")
        if not path or not Path(path).is_file():
            self.skipTest("Set G3T_GAME_DLL for read-only CP 1.75 HUD byte-contract checks")
        data = _testbuild.cached_bytes(path)
        contracts = {"RootLayout": 0x7CD3C, "ListLayout": 0x7CD96, "AppendArguments": 0x7CDAB,
                     "AppendEntry": 0xA76D0, "AppendReturn": 0xA77D2}
        for name, address in contracts.items():
            match = re.search(rf"\b{name}\[\]\s*=\s*\{{([^}}]+)\}}", self.hud)
            expected = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]+)", match[1]))
            self.assertEqual(read_rva(data, address, len(expected)), expected, name)
        # The old wrapper's activating IAT call is deliberately outside the new append entry.
        self.assertEqual(read_rva(data, 0x7CDA5, 2), b"\xff\x15")
        for address, color in ((0x7C64A, 0x103F2AAC), (0x7C912, 0x103F2A94),
                               (0x7CA6E, 0x103F2AB8), (0x7CAC6, 0x103F2A88)):
            self.assertEqual(read_rva(data, address, 5), b"\x68" + struct.pack("<I", color))
        entities_header = _testbuild.src_text("hud_entities.h")
        for name, address in (("ClearAllEntitiesWrapperBytes", 0x7C260),
                              ("DialogClearAllEntitiesBytes", 0x8B620),
                              ("SetActivePagePrefixBytes", 0x8B340)):
            match = re.search(rf"\b{name}\[\]\s*=\s*\{{([^}}]+)\}}", entities_header)
            expected = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]+)", match[1]))
            self.assertEqual(read_rva(data, address, len(expected)), expected, name)

    def test_dangling_hud_entity_slots_scrubbed_on_destroy_idle_and_world_change(self):
        result = HUD_ENTITIES_TARGET.run(timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        pvs = _testbuild.src_text("world_pvs.cpp")
        destroy = function_body(pvs, r"\bvoid\s+__fastcall\s+DestroyEntity\s*\(")
        self.assertIn("hud::OnEntityDestroyed(entity);", destroy)
        self.assertIn("npcplayback::OnEntityDestroyed(entity);", destroy)
        self.assertLess(destroy.index("hud::OnEntityDestroyed(entity);"),
                        destroy.index("originalDestroyEntity(entity, edx);"))

        tick = function_body(self.hud, r"\bvoid\s+Tick\s*\(")
        self.assertIn("ScrubIdleEntitySlots();", tick)
        self.assertLess(tick.index("ScrubIdleEntitySlots();"), tick.index("if (!list)"))

        reset = function_body(self.hud, r"\bvoid\s+OnWorldChange\s*\(")
        self.assertIn("ScrubWorldEntitySlots();", reset)
        self.assertNotIn("pending", reset)


if __name__ == "__main__":
    unittest.main()
