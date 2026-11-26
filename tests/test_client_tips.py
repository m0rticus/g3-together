import os
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
    "client_tips",
    [ROOT / "tests" / "client_tips_test.cpp"],
    allow_msvc=False,
    respect_env=False,
    skip_message="C++17 compiler required for engine-free client tips tests",
)


def function_body(text, signature):
    for match in re.finditer(signature, text):
        opening = text.find("{", match.end())
        if opening < 0 or ";" in text[match.end():opening]:
            continue
        depth = 0
        for index in range(opening, len(text)):
            if text[index] == "{":
                depth += 1
            elif text[index] == "}":
                depth -= 1
                if not depth:
                    return text[opening + 1:index]
    raise AssertionError(f"missing function {signature}")


class ClientTipsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.executable = TARGET.executable()

    def test_policy_tracker_and_native_contract(self):
        TARGET.run(check=True)

    def test_policy_header_is_engine_free(self):
        policy = _testbuild.src_text("client_tips_policy.h")
        self.assertEqual(
            re.findall(r"^\s*#\s*include\s+([^\r\n]+)", policy, re.M),
            ["<cstddef>", "<cstdint>", "<cstring>"],
        )

    def test_client_join_new_game_and_reconnect_wiring(self):
        persist_client = _testbuild.src_text("persist_client.cpp")
        new_game = function_body(persist_client, r"\bbool\s+__stdcall\s+NewGameDetour\s*\(")
        self.assertLess(new_game.index('tips::PrepareForWorldEntry("new-game")'),
                        new_game.index("const bool ok = originalNewGame(self);"))
        self.assertLess(new_game.index("const bool ok = originalNewGame(self);"),
                        new_game.index('tips::OnWorldEntered("new-game"'))

        enter = function_body(persist_client, r"\bvoid\s+EnterTemplateWorld\s*\(")
        self.assertLess(enter.index('tips::PrepareForWorldEntry("template-load")'),
                        enter.index("originalLoad(files, &name)"))
        self.assertLess(enter.index("originalLoad(files, &name)"),
                        enter.index('tips::OnWorldEntered("template-load"'))

        persist = _testbuild.src_text("persist.cpp")
        tick = function_body(persist, r"\bvoid\s+Tick\s*\(")
        self.assertIn("tips::Tick(false, false, false, nullptr, 0);", tick)
        self.assertIn("tips::Tick(false, true, running, game.GetHero(), net::WelcomeSerial());", tick)

        module = _testbuild.src_text("Script_G3Together.cpp")
        world_activate = function_body(module, r"\bbEResult\s+WorldActivate\s*\(")
        self.assertIn("g3t::tips::OnWorldActivate();", world_activate)

        tips = _testbuild.src_text("client_tips.cpp")
        self.assertIn("SetHideTips(GETrue)", tips)
        self.assertIn("checkBox->SetCheck(GETrue)", tips)
        self.assertIn("eEPropertySetType_PlayerMemory", tips)

    def test_shipped_open_tutorial_native_bytes(self):
        path = os.environ.get("G3T_GAME_DLL")
        if not path:
            self.skipTest("Set G3T_GAME_DLL for read-only native OpenTutorial verification")
        image = _testbuild.cached_pe(path)
        rva = image.exports()["?OpenTutorial@gCGUIManager@@QAE_NABVbCUnicodeString@@@Z"]
        self.assertEqual(rva, 0x7C4B0)
        code = image.get_data(rva, 29)
        self.assertEqual(
            code[:21],
            bytes.fromhex("568b710485f67436578b7e7485ff74278d4f58ff15"),
        )
        self.assertEqual(code[25:29], bytes.fromhex("84c0751a"))


if __name__ == "__main__":
    unittest.main()
