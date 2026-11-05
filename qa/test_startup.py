"""Exercise video-free QA setup without starting Gothic 3."""
import os
import tempfile
import unittest
from pathlib import Path

from startup import BACKUP_SUFFIX, disable_startup_videos


class StartupVideoTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='g3t-qa-startup-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)/'test-install'
        (self.root/'Ini').mkdir(parents=True)
        (self.root/'Data'/'Video').mkdir(parents=True)
        self.logos = self.root/'Ini'/'logo.ini'
        self.logos.write_bytes(b'G3_Logo_01.bik\r\nPublisher.bik\r\nCPT.bik\r\n')
        self.intro = self.root/'Data'/'Video'/'G3_Intro.bik'
        self.intro.write_bytes(b'intro video')
        self.credits = self.intro.with_name('G3_Credits.bik')
        self.credits.write_bytes(b'credits video')

    def test_disables_and_preserves_originals_idempotently(self):
        original_logos = self.logos.read_bytes()
        for _ in range(2):
            self.assertEqual(disable_startup_videos(self.root),
                             {'logo_list_empty': True, 'intro_file_absent': True})
            self.assertEqual(self.logos.read_bytes(), b'')
            self.assertEqual(self.logos.with_name(self.logos.name + BACKUP_SUFFIX).read_bytes(), original_logos)
            self.assertFalse(self.intro.exists())
            self.assertEqual(self.intro.with_name(self.intro.name + BACKUP_SUFFIX).read_bytes(), b'intro video')
            self.assertEqual(self.credits.read_bytes(), b'credits video')

    def test_hardlinked_logo_does_not_change_the_other_file(self):
        external = Path(self.temp.name)/'external-logo.ini'
        external.write_bytes(self.logos.read_bytes())
        self.logos.unlink()
        try:
            os.link(external, self.logos)
        except OSError as error:
            self.skipTest(f'Hardlinks unavailable: {error}')
        disable_startup_videos(self.root)
        self.assertEqual(external.read_bytes(), b'G3_Logo_01.bik\r\nPublisher.bik\r\nCPT.bik\r\n')
        self.assertEqual(self.logos.read_bytes(), b'')

    def test_hardlinked_intro_does_not_change_the_other_file(self):
        external = Path(self.temp.name)/'external-intro.bik'
        external.write_bytes(b'intro video')
        self.intro.unlink()
        try:
            os.link(external, self.intro)
        except OSError as error:
            self.skipTest(f'Hardlinks unavailable: {error}')
        disable_startup_videos(self.root)
        self.assertEqual(external.read_bytes(), b'intro video')
        self.assertEqual(self.intro.with_name(self.intro.name + BACKUP_SUFFIX).read_bytes(), b'intro video')

    def test_existing_intro_backup_does_not_get_overwritten(self):
        backup = self.intro.with_name(self.intro.name + BACKUP_SUFFIX)
        backup.write_bytes(b'previous intro')
        logos = self.logos.read_bytes()
        with self.assertRaisesRegex(RuntimeError, 'Both QA intro and its backup exist'):
            disable_startup_videos(self.root)
        self.assertEqual(self.logos.read_bytes(), logos)
        self.assertEqual(self.intro.read_bytes(), b'intro video')
        self.assertEqual(backup.read_bytes(), b'previous intro')

    def test_changed_logo_list_does_not_overwrite_backup(self):
        backup = self.logos.with_name(self.logos.name + BACKUP_SUFFIX)
        backup.write_bytes(b'other logo list')
        original = self.logos.read_bytes()
        with self.assertRaisesRegex(RuntimeError, 'differs from its existing backup'):
            disable_startup_videos(self.root)
        self.assertEqual(self.logos.read_bytes(), original)
        self.assertEqual(backup.read_bytes(), b'other logo list')
        self.assertTrue(self.intro.exists())

    def test_missing_logo_list_fails_before_moving_intro(self):
        self.logos.unlink()
        with self.assertRaisesRegex(FileNotFoundError, 'QA logo list missing'):
            disable_startup_videos(self.root)
        self.assertTrue(self.intro.exists())


if __name__ == '__main__':
    unittest.main()
