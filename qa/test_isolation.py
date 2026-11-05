"""Fail-closed isolation checks without loading any game code."""
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from isolation import FOLDER_MODULES, imports, verify_copies


class IsolationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='g3t-isolation-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.copies = {tag: self.root/tag for tag in ('host', 'client')}
        for root in self.copies.values():
            (root/'isolated-profile'/'Documents'/'gothic3').mkdir(parents=True)
            (root/'scripts').mkdir()
            for name in ('Gothic3.exe', 'g3qa32.dll', 'scripts/Script_G3Together.dll', *FOLDER_MODULES):
                (root/name).write_bytes(b'disposable test file')
        (self.root/'project.json').write_text(json.dumps({'protected_save_dir': str(self.root/'protected')}))
        self.mock_imports = patch('isolation.imports', return_value={'g3qa32.dll': ['SHGetFolderPathA']}).start()
        self.addCleanup(patch.stopall)

    def verify(self):
        return verify_copies(self.root, self.copies)

    def test_isolated_matching_copies_pass(self):
        sha, protected = self.verify()
        self.assertEqual(len(sha), 64)
        self.assertEqual(protected, self.root/'protected')

    def test_original_shell_folder_import_is_refused(self):
        self.mock_imports.return_value = {'shell32.dll': ['SHGetFolderPathA']}
        with self.assertRaisesRegex(RuntimeError, 'Unisolated folder import'):
            self.verify()

    def test_remaining_native_folder_import_is_refused(self):
        self.mock_imports.return_value = {'g3qa32.dll': ['SHGetFolderPathA'], 'shell32.dll': ['SHGetKnownFolderPath']}
        with self.assertRaisesRegex(RuntimeError, 'Native folder import remains'):
            self.verify()

    def test_protected_save_overlap_is_refused(self):
        (self.root/'project.json').write_text(json.dumps({'protected_save_dir': str(self.copies['host'])}))
        with self.assertRaisesRegex(RuntimeError, 'overlaps protected'):
            self.verify()

    def test_same_copy_for_both_roles_is_refused(self):
        self.copies['client'] = self.copies['host']
        with self.assertRaisesRegex(RuntimeError, 'separate directories'):
            self.verify()

    def test_mismatched_mods_are_refused(self):
        (self.copies['client']/'scripts'/'Script_G3Together.dll').write_bytes(b'other mod')
        with self.assertRaisesRegex(RuntimeError, 'mod DLLs differ'):
            self.verify()

    def test_hardlinked_writable_config_is_refused(self):
        outside = self.root/'real-config'
        outside.write_bytes(b'protect')
        os.link(outside, self.copies['host']/'g3together.ini')
        with self.assertRaisesRegex(RuntimeError, 'hard links'):
            self.verify()

    def test_non_executable_import_input_is_refused(self):
        with self.assertRaisesRegex(ValueError, 'Not a PE file'):
            imports(self.copies['host']/'Gothic3.exe')


if __name__ == '__main__':
    unittest.main()
