"""Exercise Steam-only package/setup behavior in disposable folders, without starting the game."""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MOD_DLL = None
STEAM_DLL = None


class SteamOnlySetupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='g3t-setup-steam-only-')
        self.root = Path(self.temp.name)
        self.package = self.root / 'package'
        self.game = self.root / 'game'
        (self.package / 'tools').mkdir(parents=True)
        (self.package / 'scripts').mkdir()
        (self.game / 'scripts').mkdir(parents=True)
        (self.game / 'Gothic3.exe').write_bytes(b'MZ')
        shutil.copy2(ROOT / 'tools' / 'setup.ps1', self.package / 'tools' / 'setup.ps1')
        shutil.copy2(MOD_DLL, self.package / 'scripts' / 'Script_G3Together.dll')

    def tearDown(self):
        self.temp.cleanup()

    def setup(self, role, bundled=True, session_id=None):
        if bundled:
            (self.package / 'G3Together').mkdir(exist_ok=True)
            shutil.copy2(STEAM_DLL, self.package / 'G3Together' / 'steam_api.dll')
        session_arg = f' -SessionId "{session_id}"' if session_id is not None else ''
        command = (
            'function global:Invoke-WebRequest { throw "offline setup fixture" }; '
            f'& "{self.package / "tools" / "setup.ps1"}" -GamePath "{self.game}" '
            f'-Role {role}{session_arg} -Yes'
        )
        return subprocess.run(
            ['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', command],
            env={key: value for key, value in os.environ.items() if key.upper() != 'PSMODULEPATH'},
            capture_output=True, text=True, timeout=30,
        )

    def test_host_setup_has_no_direct_connection_settings(self):
        result = self.setup('host')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        ini = (self.game / 'g3together.ini').read_text()
        self.assertIn('role = host', ini)
        self.assertIn('transport = steam', ini)
        self.assertIn('friendly_fire = 0', ini)
        self.assertNotRegex(ini, r'(?m)^\s*name\s*=')
        self.assertNotRegex(ini, r'(?m)^\s*(address|port)\s*=')
        installed = self.game / 'G3Together' / 'steam_api.dll'
        self.assertEqual(hashlib.sha256(installed.read_bytes()).digest(), hashlib.sha256(STEAM_DLL.read_bytes()).digest())

    def test_client_setup_removes_legacy_udp_settings(self):
        (self.game / 'g3together.ini').write_text(
            'role = client\ntransport = udp\naddress = 192.0.2.1\nport = 34567\nname = OldName\nlook = Diego\nfriendly_fire = 35\n',
        )
        result = self.setup('client')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        ini = (self.game / 'g3together.ini').read_text()
        self.assertIn('role = client', ini)
        self.assertIn('transport = steam', ini)
        self.assertIn('look = Diego', ini)
        self.assertIn('friendly_fire = 35', ini)
        self.assertNotIn('friendly_fire = 0', ini)
        self.assertNotRegex(ini, r'(?m)^\s*name\s*=')
        self.assertNotRegex(ini, r'(?m)^\s*(address|port)\s*=')
        self.assertNotRegex(ini, r'(?m)^\s*name\s*=')
        self.assertTrue(list((self.game / 'G3Together-backup').glob('g3together-*.ini')))

    def test_missing_required_steam_dll_stops_before_mod_install(self):
        original = 'role = host\nname = Existing\n'
        (self.game / 'g3together.ini').write_text(original)
        result = self.setup('host', bundled=False)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Steam is required', result.stdout)
        self.assertFalse((self.game / 'scripts' / 'Script_G3Together.dll').exists())
        self.assertEqual((self.game / 'g3together.ini').read_text(), original)

    def test_default_session_id_is_written_once(self):
        result = self.setup('host')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        ini = (self.game / 'g3together.ini').read_text()
        self.assertEqual(len(__import__('re').findall(r'(?m)^session_id\s*=', ini)), 1)
        self.assertIn('session_id = default', ini)

    def test_custom_session_id_is_written_once(self):
        result = self.setup('client', session_id='pair_A-7')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        ini = (self.game / 'g3together.ini').read_text()
        self.assertIn('session_id = pair_A-7', ini)
        self.assertEqual(len(__import__('re').findall(r'(?m)^session_id\s*=', ini)), 1)

    def test_invalid_session_id_stops_setup(self):
        for session_id in ('bad id', 'bad.id', 'A' * 64, 'pair_A-7\n'):
            with self.subTest(session_id=session_id):
                result = self.setup('host', session_id=session_id)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('Invalid session ID', result.stdout)
                self.assertFalse((self.game / 'scripts' / 'Script_G3Together.dll').exists())

    def test_existing_valid_session_id_is_preserved(self):
        (self.game / 'g3together.ini').write_text('role = host\nsession_id = preserved_pair-2\n')
        result = self.setup('host')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        ini = (self.game / 'g3together.ini').read_text()
        self.assertIn('session_id = preserved_pair-2', ini)
        self.assertEqual(len(__import__('re').findall(r'(?m)^session_id\s*=', ini)), 1)

    def test_packaged_examples_are_steam_only(self):
        output = self.root / 'packages'
        result = subprocess.run(
            [sys.executable, str(ROOT / 'tools' / 'package.py'), str(MOD_DLL), str(output), str(STEAM_DLL)],
            capture_output=True, text=True, timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        archive = next(output.glob('G3Together-*.zip'))
        with zipfile.ZipFile(archive) as package:
            for name in ('g3together.ini.host-example', 'g3together.ini.client-example'):
                ini = package.read(name).decode()
                self.assertIn('transport = steam', ini)
                self.assertIn('friendly_fire = 0', ini)
                self.assertNotIn('transport = udp', ini)
                self.assertNotRegex(ini, r'(?m)^\s*(address|port)\s*=')
            self.assertEqual(package.read('G3Together/steam_api.dll'), STEAM_DLL.read_bytes())
            self.assertNotIn('qa-enable.txt', package.namelist())
            self.assertNotIn('fake_steam_api.dll', package.namelist())


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('mod_dll', type=Path)
    parser.add_argument('steam_dll', type=Path)
    args = parser.parse_args()
    MOD_DLL, STEAM_DLL = args.mod_dll.resolve(), args.steam_dll.resolve()
    for path in (MOD_DLL, STEAM_DLL):
        if not path.is_file():
            parser.error(f'Missing fixture dependency: {path}')
    unittest.main(argv=[__file__])
