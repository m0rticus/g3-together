import re
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
import _testbuild

SAVETHUMB_TARGET = _testbuild.register_target(
    "savethumb",
    [_testbuild.unit(
        "tests/savethumb_test.cpp",
        std="c++17",
        warnings="strict",
    )],
)
PERSIST = _testbuild.src_text("persist.cpp")


def body(source, signature):
    for found in re.finditer(re.escape(signature), source):
        brace = found.end()
        while source[brace].isspace():
            brace += 1
        if source[brace] == "{":
            break
    else:
        raise AssertionError("definition not found: " + signature)
    depth = 0
    for at in range(brace, len(source)):
        if source[at] == "{":
            depth += 1
        elif source[at] == "}":
            depth -= 1
            if depth == 0:
                return source[brace:at + 1]
    raise AssertionError("unterminated function " + signature)


class SaveThumbnailTests(unittest.TestCase):
    def test_capture_policy_without_engine(self):
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            result = SAVETHUMB_TARGET.run(cwd=directory)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_thumbnail_is_chosen_at_request_and_never_recaptured_at_execution(self):
        # HUD.SaveGame passes [manager+8]; the save page is still drawn when SaveNow runs, so a capture
        # there is a picture of the menu (the reported Savegame No. 0004).
        save_now = body(PERSIST, "void CALLBACK SaveNow(HWND, UINT, UINT_PTR, DWORD)")
        self.assertNotIn("uiEntries[4]", save_now)
        self.assertNotIn("CaptureSceneImage", save_now)
        self.assertNotIn("PrepareSaveThumbnail();", save_now)

        start = body(PERSIST, "bool StartSave(char const* name)")
        prepare = start.index("PrepareSaveThumbnail();")
        self.assertLess(start.index("SetTimer(saveWindow"), prepare)
        self.assertLess(prepare, start.index("Notice(i18n::SaveQueued);"),
                        "the queued notice must not be drawn into the thumbnail")
        for refusal in re.finditer(r'CancelSave\("(?:peer-loading|identity|timer)"\)', start):
            self.assertLess(refusal.start(), prepare, "refused saves must not capture")

    def test_every_host_trigger_converges_on_start_save(self):
        # (a) menu Save, (b) quicksave, (c) client request executed by the host, and native World.SaveGameWorld callers.
        save_detour = body(PERSIST, "bool __stdcall SaveDetour(void* self, bCString const* name)")
        self.assertIn("if (net::IsHost()) { StartSave(name->GetText()); return false; }", save_detour)
        self.assertIn("SaveDetour(nullptr, &slot);",
                      body(PERSIST, "bool __fastcall GuiMessageDetour(gCGUIManager* self, void*, GEUInt message, GEUInt key, GEInt flags)"))
        self.assertIn("originalNewSlot(self);", body(PERSIST, "bool __stdcall MenuNewSlot(void* self)"))
        self.assertIn("StartSave(note.name)", body(PERSIST, "void OnSave(uint8_t from, net::SaveNote const& note)"))
        self.assertIn("return SaveDetour(nullptr, &name);",
                      body(PERSIST, "bool __fastcall WorldSaveDetour(gCWorld* self, void*, bCString const& name, eCGfxImageData* image, void* metadata, GEU32 size)"))

    def test_native_capture_is_only_called_for_the_scene_decision(self):
        calls = [m.start() for m in re.finditer(r"CaptureSceneImage\(", PERSIST)]
        definition = PERSIST.index("void CaptureSceneImage(void* manager)")
        prepare = body(PERSIST, "void PrepareSaveThumbnail()")
        self.assertEqual(len(calls), 2, "definition plus the single request-time call")
        self.assertIn("if (choice != saving::thumb::Choice::KeepRetained) CaptureSceneImage(manager);", prepare)
        template = body(PERSIST, "bool TemplateStart(rejoin::tmpl::Origin origin, TemplateJob job)")
        self.assertIn("uiEntries[4]", template)
        self.assertEqual(PERSIST.count("uiEntries[4]"), 5, "template guard/log/call, scene capture helper, and install log")
        self.assertLess(definition, PERSIST.index("void PrepareSaveThumbnail()"))

    def test_menu_open_frames_stamp_the_world_epoch(self):
        observe = body(PERSIST, "void ObserveThumbnail()")
        self.assertIn("IsMenuOpen()", observe)
        self.assertIn("thumbnail.Stamp(epoch)", observe)
        tick = body(PERSIST, "void Tick()")
        self.assertIn("ObserveThumbnail();", tick)


if __name__ == "__main__":
    unittest.main()
