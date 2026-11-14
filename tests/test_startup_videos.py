from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def load_tool(name: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


startup_videos = load_tool("startup_videos")


class StartupVideoTests(unittest.TestCase):
    def make_copy(self, root: Path) -> tuple[Path, Path]:
        ini = root / "Ini"
        video = root / "Data" / "Video"
        ini.mkdir(parents=True)
        video.mkdir(parents=True)
        logos = ini / "logo.ini"
        intro = video / "G3_Intro.bik"
        logos.write_bytes(b"LogoA\nLogoB\n")
        intro.write_bytes(b"Synthetic intro data")
        return logos, intro

    def test_disable_then_restore_round_trip(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            logos, intro = self.make_copy(root)
            original_logos = logos.read_bytes()

            startup_videos.disable(root)

            self.assertEqual(logos.read_bytes(), b"")
            self.assertFalse(intro.exists())
            self.assertTrue(logos.with_name("logo.ini.g3t-manual-video-backup").is_file())
            self.assertTrue(intro.with_name("G3_Intro.bik.g3t-manual-video-backup").is_file())

            startup_videos.restore(root)

            self.assertEqual(logos.read_bytes(), original_logos)
            self.assertTrue(intro.is_file())
            self.assertEqual(intro.read_bytes(), b"Synthetic intro data")

    def test_hard_linked_file_is_rejected_before_mutation(self):
        import os
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            root = base / "copy"
            logos, _intro = self.make_copy(root)
            shared = base / "shared-logo.ini"
            os.link(logos, shared)
            with self.assertRaisesRegex(RuntimeError, r"(?i)hard-linked"):
                startup_videos.disable(root)
            self.assertEqual(shared.read_bytes(), b"LogoA\nLogoB\n")


if __name__ == "__main__":
    unittest.main()
