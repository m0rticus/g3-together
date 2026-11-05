"""Focused Steam-only transport tests; no game installation or real Steam account is used."""
import argparse
import ast
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

ARTIFACTS = None


class SteamOnlyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='g3t-steam-only-')
        self.root = Path(self.temp.name)
        self.processes = []
        self.logs = []
        self.base = 100 + os.getpid() % 700

    def tearDown(self):
        for process in self.processes:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
        for log in self.logs:
            log.close()
        self.temp.cleanup()

    def environment(self, host, **extra):
        env = dict(os.environ)
        for key in list(env):
            if key.startswith('G3T_FAKESTEAM_') or key == 'G3T_STEAM_DLL':
                del env[key]
        env.update(
            G3T_STEAM_DLL=str(ARTIFACTS / 'fake_steam_api.dll'),
            G3T_FAKESTEAM_DIR=str(self.root),
            G3T_FAKESTEAM_ID=str(self.base + (0 if host else 1)),
            G3T_FAKESTEAM_NAME='SteamOnlyHost' if host else 'SteamOnlyClient',
        )
        env.update(extra)
        return env

    def start(self, role, env, seconds=25, steam_host='SteamOnlyHost', extra_args=()):
        path = self.root / f'{role}-{len(self.processes)}.log'
        log = path.open('w', encoding='utf-8')
        self.logs.append(log)
        process = subprocess.Popen(
            [str(ARTIFACTS / 'g3t_relaytest.exe'), role, str(seconds), steam_host, *extra_args],
            env=env, stdout=log, stderr=subprocess.STDOUT,
        )
        self.processes.append(process)
        return process, path

    def wait_for_lobby(self, process, path):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and process.poll() is None:
            match = re.search(r'steam: lobby (\d+) open;', path.read_text(encoding='utf-8'))
            if match:
                return match[1]
            time.sleep(0.05)
        self.fail(path.read_text(encoding='utf-8'))

    def test_three_pairs_share_discovery_but_not_gameplay(self):
        pairs = []
        for index, session in enumerate(('pair-red', 'pair-green', 'pair-blue')):
            host_name = f'Host-{session}'
            host, host_log = self.start('host', self.environment(
                True, G3T_FAKESTEAM_ID=str(self.base + index * 2),
                G3T_FAKESTEAM_NAME=host_name, G3T_RELAY_SESSION_ID=session,
            ))
            lobby_id = self.wait_for_lobby(host, host_log)
            pairs.append((session, host_name, lobby_id, host, host_log))
        clients = []
        # Reverse start order and no steam_host filter: only session_id can select the right lobby.
        for index in reversed(range(3)):
            session, host_name, lobby_id, _, _ = pairs[index]
            client_name = f'Client-{session}'
            client, client_log = self.start('client', self.environment(
                False, G3T_FAKESTEAM_ID=str(self.base + index * 2 + 1),
                G3T_FAKESTEAM_NAME=client_name, G3T_RELAY_SESSION_ID=session,
            ), steam_host='')
            clients.append((index, client_name, client, client_log))
        for index, client_name, client, client_log in clients:
            session, host_name, lobby_id, host, host_log = pairs[index]
            for process, path in ((client, client_log), (host, host_log)):
                process.wait(timeout=35)
                text = path.read_text(encoding='utf-8')
                self.assertEqual(process.returncode, 0, text)
                self.assertIn('RESULT: PASS', text)
                self.assertIn(f'Session ID: {session}', text)
                self.assertNotIn('cross-session', text)
            client_text = client_log.read_text(encoding='utf-8')
            host_text = host_log.read_text(encoding='utf-8')
            self.assertIn(f'host Steam wire name={host_name}', client_text)
            self.assertEqual(re.findall(r'steam: joining lobby (\d+)', client_text), [lobby_id])
            self.assertIn(f'net: {client_name} joined as player 2', host_text)
            self.assertEqual(len(re.findall(r'joined as player', host_text)), 1, host_text)
            self.assertIn('save 2097152 of 2097152 bytes, crc ok', client_text)

    def test_mismatch_blocks_search_explicit_lobby_command_line_and_invite(self):
        host, host_log = self.start('lobbyprobe', self.environment(True, G3T_RELAY_SESSION_ID='Pair-A'), seconds=10)
        lobby_id = self.wait_for_lobby(host, host_log)
        clients = []
        for index, route in enumerate(('search', 'explicit', 'command_line', 'invite')):
            extra = dict(G3T_RELAY_SESSION_ID='pair-a', G3T_FAKESTEAM_ID=str(self.base + index + 1))
            args = ()
            if route == 'explicit':
                extra['G3T_RELAY_STEAM_LOBBY'] = lobby_id
            elif route == 'command_line':
                args = ('+connect_lobby', lobby_id)
            elif route == 'invite':
                extra['G3T_FAKESTEAM_INVITE'] = lobby_id
            clients.append(self.start('client', self.environment(False, **extra), seconds=4,
                                      steam_host='', extra_args=args))
        for client, path in clients:
            client.wait(timeout=12)
            text = path.read_text(encoding='utf-8')
            self.assertNotEqual(client.returncode, 0, text)
            self.assertIn('different session_id or incompatible transport', text)
            self.assertNotIn('steam: joining lobby', text)
            self.assertIn('welcomed 0, host state 0, save 0 of', text)
        self.assertNotIn('linked to this game', host_log.read_text(encoding='utf-8'))
        self.assertFalse(list(self.root.glob(f'member_{lobby_id}_*')))

    def test_relay_envelopes_reject_wrong_session_and_stale_lobby(self):
        host, host_log = self.start('lobbyprobe', self.environment(True), seconds=10)
        self.wait_for_lobby(host, host_log)
        clients = []
        for index, tamper in enumerate(('SESSION', 'LOBBY', 'DATA_SESSION', 'DATA_LOBBY')):
            clients.append((tamper, *self.start('client', self.environment(
                False, G3T_FAKESTEAM_ID=str(self.base + index + 1),
                **{f'G3T_FAKESTEAM_TAMPER_{tamper}': '1'},
            ), seconds=4)))
        for tamper, client, path in clients:
            client.wait(timeout=12)
            text = path.read_text(encoding='utf-8')
            self.assertNotEqual(client.returncode, 0, text)
            self.assertIn('welcomed 0, host state 0, save 0 of', text)
            if tamper.startswith('DATA_'):
                self.assertIn('welcomed by the host', text)
            else:
                self.assertNotIn('welcomed by the host', text)
        host.wait(timeout=15)
        text = host_log.read_text(encoding='utf-8')
        self.assertEqual(host.returncode, 0, text)
        self.assertRegex(text, r'isolated_drops=[1-9]\d*')
        self.assertNotIn('joined as player', text)

    def test_longest_session_id_connects(self):
        session = 'X' * 63
        host, host_log = self.start('host', self.environment(True, G3T_RELAY_SESSION_ID=session))
        self.wait_for_lobby(host, host_log)
        client, client_log = self.start('client', self.environment(False, G3T_RELAY_SESSION_ID=session))
        for process, path in ((client, client_log), (host, host_log)):
            process.wait(timeout=30)
            self.assertEqual(process.returncode, 0, path.read_text(encoding='utf-8'))

    def test_legacy_relay_lobby_is_not_joined(self):
        lobby_id = str(self.base)
        (self.root / f'lobby_{lobby_id}.txt').write_text(
            'g3together=1\npersona=SteamOnlyHost\nsession_id=default\napp_id=39500\n',
            encoding='utf-8',
        )
        client, path = self.start('client', self.environment(False), seconds=4)
        client.wait(timeout=12)
        text = path.read_text(encoding='utf-8')
        self.assertNotEqual(client.returncode, 0, text)
        self.assertIn('incompatible transport', text)
        self.assertNotIn('steam: joining lobby', text)
        self.assertIn('welcomed 0, host state 0, save 0 of', text)

    def test_invalid_session_id_fails_without_default_lobby(self):
        for session in ('', 'with space', 'x' * 64, 'non-ascii-\u00e4', 'path/name'):
            for role in ('host', 'client'):
                with self.subTest(session=session, role=role):
                    process, path = self.start(role, self.environment(
                        role == 'host', G3T_RELAY_SESSION_ID=session,
                    ), seconds=1)
                    process.wait(timeout=10)
                    text = path.read_text(encoding='utf-8')
                    self.assertNotEqual(process.returncode, 0, text)
                    self.assertIn('invalid session_id', text)
                    self.assertNotIn('steam: loaded', text)

    def test_ini_session_id_validation_and_legacy_default(self):
        scripts = self.root / 'scripts'
        scripts.mkdir()
        executable = scripts / 'g3t_config_test.exe'
        shutil.copy2(ARTIFACTS / executable.name, executable)
        ini = self.root / 'g3together.ini'
        for value, expected in (
            (None, 'default'), ('pair-A_123', 'pair-A_123'), ('"pair-A"', 'pair-A'),
            ('x' * 63, 'x' * 63), ('', '!invalid'), ('x' * 64, '!invalid'),
            ('with space', '!invalid'), ('bad/name', '!invalid'), ('\u00e4', '!invalid'),
            ('x' * 300, '!invalid'),
        ):
            with self.subTest(value=value):
                ini.write_text('role = client\n' + ('' if value is None else f'session_id = {value}\n'),
                               encoding='utf-8')
                result = subprocess.run([str(executable), expected], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_steam_round_trip_loopback_and_direct_rejection(self):
        host, host_log = self.start('host', self.environment(True))
        deadline = time.monotonic() + 10
        port = None
        while time.monotonic() < deadline and host.poll() is None:
            text = host_log.read_text(encoding='utf-8')
            match = re.search(r'Steam-only host relay on 127\.0\.0\.1:(\d+)', text)
            if match:
                port = int(match[1])
                break
            time.sleep(0.05)
        self.assertIsNotNone(port, host_log.read_text(encoding='utf-8'))
        command = (
            f"@(Get-NetUDPEndpoint -OwningProcess {host.pid} -ErrorAction Stop | "
            "Select-Object -ExpandProperty LocalAddress) | ConvertTo-Json -Compress"
        )
        endpoints = subprocess.run(
            ['powershell.exe', '-NoProfile', '-Command', command],
            capture_output=True, text=True, check=True, timeout=15,
        )
        addresses = json.loads(endpoints.stdout)
        self.assertTrue(addresses)
        self.assertTrue(all(address == '127.0.0.1' for address in addresses), addresses)
        direct = subprocess.run(
            [str(ARTIFACTS / 'g3t_relaytest.exe'), 'direct', str(port), '3'],
            env=self.environment(False), capture_output=True, text=True, timeout=10,
        )
        self.assertEqual(direct.returncode, 0, direct.stdout + direct.stderr)
        self.assertIn('welcomed 0', direct.stdout)
        client, client_log = self.start('client', self.environment(False))
        client.wait(timeout=25)
        host.wait(timeout=25)
        host_text = host_log.read_text(encoding='utf-8')
        client_text = client_log.read_text(encoding='utf-8')
        self.assertEqual(client.returncode, 0, client_text)
        self.assertEqual(host.returncode, 0, host_text)
        self.assertIn('rejected an ENet connection outside the linked Steam relays', host_text)
        self.assertIn('RESULT: PASS', client_text)
        self.assertIn('RESULT: PASS', host_text)
        self.assertIn('initialized Gothic 3 AppID 39500', host_text)
        self.assertIn('net: player name from Steam: SteamOnlyHost', host_text)
        self.assertIn('net: player name from Steam: SteamOnlyClient', client_text)
        self.assertIn('Clients: 1 | ready: 0', host_text)
        self.assertIn('Clients: 1 | ready: 1', host_text)
        self.assertIn("Waiting for the host's campaign", client_text)
        self.assertIn('Campaign ready', client_text)
        self.assertIn('STEAMSTATUS app=39500', host_text)
        self.assertIn('256 canonical bones before NPC state', client_text)

    def test_missing_steam_fails_without_a_fallback(self):
        for role in ('host', 'client'):
            with self.subTest(role=role):
                process, log = self.start(role, self.environment(role == 'host', G3T_FAKESTEAM_INIT='2'))
                process.wait(timeout=10)
                text = log.read_text(encoding='utf-8')
                self.assertNotEqual(process.returncode, 0, text)
                self.assertIn('Steam is not running', text)
                self.assertIn('connected=0 joined=0', text)
                self.assertNotIn('connecting to the address', text)

    def test_sustained_native_pose_bursts_preserve_every_actor(self):
        host, host_log = self.start('host', self.environment(True, G3T_RELAY_POSE_FLOOD='1'))
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and host.poll() is None:
            if re.search(r'steam: lobby \d+ open;', host_log.read_text(encoding='utf-8')):
                break
            time.sleep(0.05)
        else:
            self.fail(host_log.read_text(encoding='utf-8'))
        client, client_log = self.start('client', self.environment(False, G3T_RELAY_POSE_FLOOD='1'))
        for process, path in ((client, client_log), (host, host_log)):
            process.wait(timeout=30)
            self.assertEqual(process.returncode, 0, path.read_text(encoding='utf-8') +
                             '\nHOST:\n' + host_log.read_text(encoding='utf-8'))
        self.assertRegex(client_log.read_text(encoding='utf-8'), r'POSEFLOOD actors=73 minimum=\d+ maximum=\d+')

    def test_discarded_fragments_release_the_waiting_budget(self):
        process, path = self.start('queueprobe', self.environment(True), seconds=1)
        process.wait(timeout=10)
        text = path.read_text(encoding='utf-8')
        self.assertEqual(process.returncode, 0, text)
        self.assertIn('discarded fragments release their exact waiting budget', text)

    def test_pose_sequence_wrap_never_promotes_a_skeletal_packet_to_reliable(self):
        host, host_log = self.start('host', self.environment(True, G3T_RELAY_POSE_WRAP='1'))
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and host.poll() is None:
            if re.search(r'steam: lobby \d+ open;', host_log.read_text(encoding='utf-8')):
                break
            time.sleep(0.05)
        else:
            self.fail(host_log.read_text(encoding='utf-8'))
        client, client_log = self.start('client', self.environment(False))
        for process, path in ((client, client_log), (host, host_log)):
            process.wait(timeout=30)
            self.assertEqual(process.returncode, 0, path.read_text(encoding='utf-8'))
        self.assertIn('pose sequence checkpoint queued; the pose stream moved to channel 5', host_log.read_text(encoding='utf-8'))
        self.assertIn('256 canonical bones before NPC state, fragmented without reliability escalation',
                      client_log.read_text(encoding='utf-8'))

    def test_steam_preserves_engine_floating_point_state(self):
        process, log = self.start('lobbyprobe', self.environment(True, G3T_FAKESTEAM_FPU='1', G3T_RELAY_CHECK_FP='1'), seconds=1)
        process.wait(timeout=10)
        text = log.read_text(encoding='utf-8')
        self.assertEqual(process.returncode, 0, text)
        self.assertIn('restoring engine FP state after startup', text)
        self.assertIn('restoring engine FP state after callbacks', text)
        self.assertIn('FPSTATE preserved=1', text)

    def test_authoritative_hit_acknowledgement_preserves_contact_identity(self):
        host, host_log = self.start('host', self.environment(True, G3T_RELAY_HIT_REDIRECT='1'))
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and host.poll() is None:
            if re.search(r'steam: lobby \d+ open;', host_log.read_text(encoding='utf-8')):
                break
            time.sleep(0.05)
        else:
            self.fail(host_log.read_text(encoding='utf-8'))
        client, client_log = self.start('client', self.environment(False, G3T_RELAY_HIT_REDIRECT='1'))
        for process, path in ((client, client_log), (host, host_log)):
            process.wait(timeout=30)
            self.assertEqual(process.returncode, 0, path.read_text(encoding='utf-8'))
        self.assertIn('acknowledged hit sequence=1 incarnation=1 accepted=1', client_log.read_text(encoding='utf-8'))

    def test_only_host_npc_publications_reach_a_third_player(self):
        host, host_log = self.start('host', self.environment(True, G3T_RELAY_AUTHORITY='1'))
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and host.poll() is None:
            if re.search(r'steam: lobby \d+ open;', host_log.read_text(encoding='utf-8')):
                break
            time.sleep(0.05)
        else:
            self.fail(host_log.read_text(encoding='utf-8'))
        client, client_log = self.start('client', self.environment(False, G3T_RELAY_AUTHORITY='1'))
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and host.poll() is None:
            if 'host: client 2 joined' in host_log.read_text(encoding='utf-8'):
                break
            time.sleep(0.05)
        else:
            self.fail('HOST:\n' + host_log.read_text(encoding='utf-8') +
                      '\nCLIENT:\n' + client_log.read_text(encoding='utf-8'))
        observer_env = self.environment(False, G3T_FAKESTEAM_ID=str(self.base + 2), G3T_FAKESTEAM_NAME='Observer')
        observer, observer_log = self.start('observer', observer_env)
        for process, path in ((observer, observer_log), (client, client_log), (host, host_log)):
            process.wait(timeout=25)
            self.assertEqual(process.returncode, 0, path.read_text(encoding='utf-8'))
        self.assertIn('refused NPC leaked 0', observer_log.read_text(encoding='utf-8'))
        self.assertIn('host death 1, refused death leaked 0', observer_log.read_text(encoding='utf-8'))
        self.assertIn('host removal 1', observer_log.read_text(encoding='utf-8'))
        self.assertIn('native defaults 1', observer_log.read_text(encoding='utf-8'))
        self.assertIn('death claims 0', host_log.read_text(encoding='utf-8'))

    def test_missing_steam_dll_fails_without_a_fallback(self):
        for role in ('host', 'client'):
            with self.subTest(role=role):
                process, log = self.start(
                    role, self.environment(role == 'host', G3T_STEAM_DLL=str(self.root / 'missing.dll')),
                )
                process.wait(timeout=10)
                text = log.read_text(encoding='utf-8')
                self.assertNotEqual(process.returncode, 0, text)
                self.assertIn('steam_api.dll not found', text)
                self.assertIn('connected=0 joined=0', text)
                self.assertNotIn('connecting to the address', text)

    def test_menu_status_formatting(self):
        result = subprocess.run(
            [str(ARTIFACTS / 'g3t_relaytest.exe'), 'menucheck'],
            capture_output=True, text=True, timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('RESULT: PASS', result.stdout)

    def test_wrong_steam_app_id_fails_explicitly(self):
        process, log = self.start('host', self.environment(True, G3T_FAKESTEAM_APPID='480'))
        process.wait(timeout=10)
        text = log.read_text(encoding='utf-8')
        self.assertNotEqual(process.returncode, 0, text)
        self.assertIn('unexpected game AppID', text)
        self.assertIn('connected=0 joined=0', text)

    def test_steam_name_truncation_preserves_utf8(self):
        name = 'A' * 22 + '😀'
        process, log = self.start('host', self.environment(True, G3T_FAKESTEAM_NAME=name), seconds=1)
        process.wait(timeout=10)
        text = log.read_text(encoding='utf-8')
        self.assertIn('net: player name from Steam: ' + 'A' * 22 + '\n', text)
        self.assertNotIn('\ufffd', text)

    @unittest.skipUnless(sys.version_info >= (3, 12), 'The upstream QA syntax requires Python 3.12+.')
    def test_qa_netsim_impairs_the_steam_transport(self):
        source = Path(__file__).resolve().parents[2] / 'qa' / 'together.py'
        tree = ast.parse(source.read_text())
        selected = [
            node for node in tree.body
            if isinstance(node, (ast.ClassDef, ast.FunctionDef)) and node.name in ('NetSim', 'fake_steam_network')
        ]
        self.assertEqual(len(selected), 2)
        pair = self.base // 10
        self.base = pair * 10 + 1
        port = 41000 + self.base
        scope = {'PORT': port, 'RELAY_PORT': port + 10, 'PAIR': str(pair), 'time': time, 'threading': threading}
        exec(compile(ast.Module(body=selected, type_ignores=[]), str(source), 'exec'), scope)
        relay = scope['NetSim']('20,5,1')
        scope['NETLINK'] = relay
        try:
            host_env = self.environment(True, **scope['fake_steam_network']('host'))
            client_env = self.environment(False, **scope['fake_steam_network']('client'))
            host, host_log = self.start('host', host_env)
            time.sleep(0.2)
            client, client_log = self.start('client', client_env)
            client.wait(timeout=25)
            host.wait(timeout=25)
            self.assertEqual(client.returncode, 0, client_log.read_text(encoding='utf-8'))
            self.assertEqual(host.returncode, 0, host_log.read_text(encoding='utf-8'))
            self.assertGreater(relay.stats['up']['packets'], 0)
            self.assertGreater(relay.stats['down']['packets'], 0)
            self.assertLessEqual(max(row['max_bytes'] for row in relay.stats.values()), 1281)
        finally:
            relay.stop()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('artifacts', type=Path, help='Folder containing g3t_relaytest.exe and fake_steam_api.dll')
    args = parser.parse_args()
    ARTIFACTS = args.artifacts.resolve()
    for name in ('g3t_relaytest.exe', 'fake_steam_api.dll', 'g3t_config_test.exe'):
        if not (ARTIFACTS / name).is_file():
            parser.error(f'Missing transport test artifact: {name}')
    unittest.main(argv=[__file__])
