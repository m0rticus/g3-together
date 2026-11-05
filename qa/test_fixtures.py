"""Fixture lifecycle checks without launching the game."""
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from fixtures import ALTERNATIVE_EXTENSIONS, EXTENSIONS, digest, publish_fixture, read_fixture, restore_fixture


class FixtureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='g3t-fixture-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.slot = 'G3T_Sandbox'
        self.path = self.root/'fixtures'/'sandbox-p1'
        self.folders = {tag: self.root/tag for tag in ('host', 'client')}
        for folder in self.folders.values():
            folder.mkdir()
        for extension in EXTENSIONS:
            (self.folders['host']/(self.slot+extension)).write_bytes(b'native save '+extension.encode())
        self.record = f'g3t-hero-G3T_host_{self.slot}.txt'
        (self.folders['client']/self.record).write_text('G3THERO 2\nend 0\n')
        self.meta = {'native_files': {'Game.dll': 'native-hash'}, 'save_reload_verified': True,
                     'baseline': {tag: {'pos': [1, 2, 3], 'dump': 'hero'} for tag in self.folders}}

    def publish(self):
        return publish_fixture(self.path, self.slot, self.folders, self.meta)

    def restore(self):
        return restore_fixture(self.path, self.slot, self.meta['native_files'], self.folders)

    def edit_meta(self, change):
        path = self.path/'meta.json'
        meta = json.loads(path.read_text())
        change(meta)
        path.write_text(json.dumps(meta))

    def test_roundtrip_restores_both_profiles_without_changing_baseline(self):
        meta = self.publish()
        before = digest(self.path/'meta.json')
        for folder in self.folders.values():
            for file in folder.iterdir():
                file.write_bytes(b'mutated scenario')
            (folder/'unrelated-save.g3savcpx').write_bytes(b'keep')
        self.assertEqual(self.restore(), meta)
        for tag, files in meta['files'].items():
            for name, sha in files.items():
                self.assertEqual(digest(self.folders[tag]/name), sha)
                self.assertEqual(digest(self.path/tag/name), sha)
            self.assertEqual((self.folders[tag]/'unrelated-save.g3savcpx').read_bytes(), b'keep')
        self.assertEqual(digest(self.path/'meta.json'), before)

    def test_missing_native_file_does_not_replace_good_fixture(self):
        self.publish()
        before = digest(self.path/'meta.json')
        (self.folders['host']/(self.slot+EXTENSIONS[0])).unlink()
        with self.assertRaisesRegex(RuntimeError, 'save or hero record missing'):
            self.publish()
        self.assertEqual(digest(self.path/'meta.json'), before)
        self.restore()

    def test_alternative_balancing_save_format_is_supported(self):
        for extension in EXTENSIONS:
            (self.folders['host']/(self.slot+extension)).unlink()
        for extension in ALTERNATIVE_EXTENSIONS:
            (self.folders['host']/(self.slot+extension)).write_bytes(b'alternative save')
        self.publish()
        self.restore()
        self.assertTrue((self.path/'host'/(self.slot+'.g3savcpxdat')).is_file())

    def test_two_native_formats_in_one_fixture_are_refused(self):
        for extension in ALTERNATIVE_EXTENSIONS:
            (self.folders['host']/(self.slot+extension)).write_bytes(b'conflicting save')
        with self.assertRaisesRegex(RuntimeError, 'save or hero record missing'):
            self.publish()

    def test_missing_client_record_is_refused(self):
        (self.folders['client']/self.record).unlink()
        with self.assertRaisesRegex(RuntimeError, 'files missing'):
            self.publish()
        self.assertFalse(self.path.exists())

    def test_corruption_checked_before_either_profile_changes(self):
        self.publish()
        host_file = self.folders['host']/(self.slot+EXTENSIONS[0])
        host_file.write_bytes(b'current scenario')
        (self.path/'client'/self.record).write_bytes(b'damaged')
        with self.assertRaisesRegex(RuntimeError, 'missing or damaged'):
            self.restore()
        self.assertEqual(host_file.read_bytes(), b'current scenario')

    def test_native_build_mismatch_is_refused(self):
        self.publish()
        with self.assertRaisesRegex(RuntimeError, 'native game build differs'):
            read_fixture(self.path, self.slot, {'Game.dll': 'another-game'})

    def test_unverified_fixture_is_refused(self):
        self.meta['save_reload_verified'] = False
        with self.assertRaisesRegex(RuntimeError, 'real save/load check'):
            self.publish()

    def test_wrong_schema_and_slot_are_refused(self):
        self.publish()
        self.edit_meta(lambda m: m.update(schema=0))
        with self.assertRaisesRegex(RuntimeError, 'Unsupported fixture'):
            self.restore()
        self.publish()
        self.edit_meta(lambda m: m.update(slot='another-slot'))
        with self.assertRaisesRegex(RuntimeError, 'Unsupported fixture'):
            self.restore()

    def test_traversal_is_refused(self):
        self.publish()
        self.edit_meta(lambda m: m['files']['host'].update({'..\\G3T_Sandbox-outside': 'hash'}))
        with self.assertRaisesRegex(RuntimeError, 'Unsafe fixture file name'):
            self.restore()

    def test_empty_files_are_refused(self):
        (self.folders['host']/(self.slot+EXTENSIONS[0])).write_bytes(b'')
        with self.assertRaisesRegex(RuntimeError, 'missing or damaged'):
            self.publish()

    def test_failed_replacement_preserves_previous_fixture(self):
        self.publish()
        before = digest(self.path/'meta.json')
        original = Path.rename

        def fail_new(path, target):
            if path.name == 'new':
                raise OSError('simulated replacement failure')
            return original(path, target)

        with patch.object(Path, 'rename', fail_new), self.assertRaisesRegex(OSError, 'simulated'):
            self.publish()
        self.assertEqual(digest(self.path/'meta.json'), before)
        self.restore()

    def test_restoring_over_hardlink_cannot_modify_external_data(self):
        self.publish()
        external = self.root/'external-save'
        external.write_bytes(b'keep this external file')
        target = self.folders['host']/(self.slot+EXTENSIONS[0])
        target.unlink()
        os.link(external, target)
        self.restore()
        self.assertEqual(external.read_bytes(), b'keep this external file')
        self.assertNotEqual(target.read_bytes(), external.read_bytes())


if __name__ == '__main__':
    unittest.main()
