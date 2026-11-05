"""Focused Steam-only transport tests; no game installation or real Steam account is used."""
import argparse
import ast
import json
import os
import re
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

    def start(self, role, env, seconds=25):
        path = self.root / f'{role}-{len(self.processes)}.log'
        log = path.open('w', encoding='utf-8')
        self.logs.append(log)
        process = subprocess.Popen(
            [str(ARTIFACTS / 'g3t_relaytest.exe'), role, str(seconds), 'SteamOnlyHost'],
            env=env, stdout=log, stderr=subprocess.STDOUT,
        )
        self.processes.append(process)
        return process, path

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
            self.assertLessEqual(max(row['max_bytes'] for row in relay.stats.values()), 1209)
        finally:
            relay.stop()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('artifacts', type=Path, help='Folder containing g3t_relaytest.exe and fake_steam_api.dll')
    args = parser.parse_args()
    ARTIFACTS = args.artifacts.resolve()
    for name in ('g3t_relaytest.exe', 'fake_steam_api.dll'):
        if not (ARTIFACTS / name).is_file():
            parser.error(f'Missing transport test artifact: {name}')
    unittest.main(argv=[__file__])
