"""Check sandbox routing without installed game copies."""
import importlib
import inspect
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


class SandboxRoutingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='g3t-sandbox-routing-')
        root = Path(cls.temp.name)
        (root/'project.json').write_text(json.dumps({'protected_save_dir': str(root/'protected')}))
        with patch.dict(os.environ, {'G3_WORKSPACE': str(root), 'G3_PAIR': '1'}):
            cls.runner = importlib.import_module('together')
            cls.session = importlib.import_module('session')

    @classmethod
    def tearDownClass(cls):
        for name in ('together', 'session', 'workspace', 'bot'):
            sys.modules.pop(name, None)
        cls.temp.cleanup()

    def setUp(self):
        self.addCleanup(patch.stopall)
        patch.dict(os.environ, {'G3_SANDBOX': ''}).start()
        self.ready = patch.object(self.runner, 'sandbox_ready', return_value={'baseline': {}}).start()
        patch.object(self.runner, 'postraid_ready', return_value=False).start()

    def test_generic_stages_default_to_saved_sandbox(self):
        for name in self.runner.SANDBOX_STAGES:
            with self.subTest(stage=name):
                self.assertEqual(self.runner.postraid_for(name), 'sandbox')

    def test_missing_fixture_does_not_fall_back_to_new_game(self):
        self.ready.side_effect = RuntimeError('Sandbox fixture missing')
        with self.assertRaisesRegex(RuntimeError, 'fixture missing'):
            self.runner.postraid_for('stageW2')

    def test_creation_and_startup_still_use_new_game(self):
        for name in ('sandboxsave', 'stageStartup', 'postraidsave', 'stageRaid', 'stageS3'):
            with self.subTest(stage=name):
                self.assertEqual(self.runner.postraid_for(name), '')
        self.ready.assert_not_called()

    def test_town_and_quest_stages_are_not_silently_moved(self):
        for name in ('stageClientFriendlyHits', 'stagePersonalEconomy', 'stageEnterPlace'):
            with self.subTest(stage=name):
                self.assertEqual(self.runner.postraid_for(name), '')
        self.ready.assert_not_called()

    def test_legacy_postraid_route_is_preserved(self):
        with patch.object(self.runner, 'postraid_ready', return_value=True):
            self.assertEqual(self.runner.postraid_for('stageEnterPlace'), 'postraid')

    def test_unsafe_global_override_is_rejected(self):
        with patch.dict(os.environ, {'G3_SANDBOX': '1'}):
            with self.assertRaisesRegex(RuntimeError, 'not been declared sandbox-safe'):
                self.runner.postraid_for('stagePersonalEconomy')

    def test_explicit_opt_out_preserves_legacy_setup(self):
        with patch.dict(os.environ, {'G3_SANDBOX': '0'}):
            self.assertEqual(self.runner.postraid_for('stageW2'), '')
        self.ready.assert_not_called()

    def test_sandbox_e2e_cannot_be_run_from_new_game(self):
        with patch.dict(os.environ, {'G3_SANDBOX': '0'}):
            with self.assertRaisesRegex(RuntimeError, 'requires the saved sandbox'):
                self.runner.postraid_for('sandboxcheck')

    def test_shared_runner_cannot_leak_sandbox_world_between_stages(self):
        for name in self.runner.SANDBOX_STAGES:
            with self.subTest(stage=name):
                self.assertFalse(self.session.shareable(name))

    def test_fixture_builders_always_run_alone(self):
        for name in ('sandboxsave', 'postraidsave'):
            with self.subTest(stage=name):
                self.assertFalse(self.session.shareable(name))

    def test_join_and_clear_setup_preserves_order_and_return_values(self):
        host = SimpleNamespace(tag='host')
        client = SimpleNamespace(tag='client')
        copies = {'host': host, 'client': client}
        report = {}
        events = []
        with patch.object(self.runner, 'join_both',
                          side_effect=lambda value: events.append(('join', value)) or (host, client)), \
                patch.object(self.runner, 'clear_raid',
                             side_effect=lambda value, result: events.append(('clear', value, result))):
            result = self.runner.join_both_and_clear(copies, report)
        self.assertEqual(result, (host, client))
        self.assertEqual(events, [('join', copies), ('clear', host, report)])

    def test_join_and_clear_propagates_join_and_clear_failures(self):
        copies = {'host': object(), 'client': object()}
        report = {}
        with patch.object(self.runner, 'join_both', side_effect=RuntimeError('join failed')), \
                patch.object(self.runner, 'clear_raid') as clear:
            with self.assertRaisesRegex(RuntimeError, 'join failed'):
                self.runner.join_both_and_clear(copies, report)
        clear.assert_not_called()

        host = SimpleNamespace(tag='host')
        with patch.object(self.runner, 'join_both', return_value=(host, SimpleNamespace(tag='client'))), \
                patch.object(self.runner, 'clear_raid', side_effect=RuntimeError('clear failed')):
            with self.assertRaisesRegex(RuntimeError, 'clear failed'):
                self.runner.join_both_and_clear(copies, report)

    def test_raid_help_up_preserves_client_first_order_and_mutates_report(self):
        host = SimpleNamespace(tag='host', bot=SimpleNamespace(state=' down=1 '))
        client = SimpleNamespace(tag='client', bot=SimpleNamespace(state=' down=1 '))
        report = {}
        events = []

        def recover(helper, downed, result, tag):
            events.append(('help', helper.tag, downed.tag, tag))
            downed.bot.state = ' down=0 '
            result[tag+'_helper_moved'] = 'recovered'

        def wait(predicate, seconds, diagnostic):
            events.append(('wait', seconds, diagnostic, predicate()))
            return predicate()

        with patch.object(self.runner, 'herostate', side_effect=lambda bot: bot.state), \
                patch.object(self.runner, 'help_up', side_effect=recover), \
                patch.object(self.runner, 'wait_for', side_effect=wait):
            self.runner.help_up_raid((client, host), report)

        self.assertEqual(events, [
            ('help', 'host', 'client', 'client_raid'),
            ('wait', 30, 'client helped up after the raid', ['up']),
            ('help', 'client', 'host', 'host_raid'),
            ('wait', 30, 'host helped up after the raid', ['up']),
        ])
        self.assertEqual(report, {
            'client_raid_helper_moved': 'recovered',
            'host_raid_helper_moved': 'recovered',
        })

    def test_raid_help_up_preserves_host_first_order(self):
        host = SimpleNamespace(tag='host', bot=SimpleNamespace(state=' down=1 '))
        client = SimpleNamespace(tag='client', bot=SimpleNamespace(state=' down=1 '))
        events = []

        with patch.object(self.runner, 'herostate', side_effect=lambda bot: bot.state), \
                patch.object(self.runner, 'help_up',
                             side_effect=lambda helper, downed, report, tag:
                             events.append((helper.tag, downed.tag, tag))), \
                patch.object(self.runner, 'wait_for',
                             side_effect=lambda predicate, seconds, diagnostic:
                             ['up'] if predicate() else None):
            self.runner.help_up_raid((host, client), {})

        self.assertEqual(events, [
            ('client', 'host', 'host_raid'),
            ('host', 'client', 'client_raid'),
        ])

    def test_raid_help_up_skips_already_standing_players(self):
        host = SimpleNamespace(tag='host', bot=SimpleNamespace(state=' down=0 '))
        client = SimpleNamespace(tag='client', bot=SimpleNamespace(state=' down=0 '))
        with patch.object(self.runner, 'herostate', side_effect=lambda bot: bot.state), \
                patch.object(self.runner, 'help_up') as help_up, \
                patch.object(self.runner, 'wait_for') as wait:
            self.runner.help_up_raid((client, host), {})
        help_up.assert_not_called()
        wait.assert_not_called()

    def test_raid_help_up_propagates_help_and_wait_failures(self):
        host = SimpleNamespace(tag='host', bot=SimpleNamespace(state=' down=1 '))
        client = SimpleNamespace(tag='client', bot=SimpleNamespace(state=' down=1 '))
        with patch.object(self.runner, 'herostate', side_effect=lambda bot: bot.state), \
                patch.object(self.runner, 'help_up', side_effect=RuntimeError('help failed')), \
                patch.object(self.runner, 'wait_for') as wait:
            with self.assertRaisesRegex(RuntimeError, 'help failed'):
                self.runner.help_up_raid((client, host), {})
        wait.assert_not_called()

        with patch.object(self.runner, 'herostate', side_effect=lambda bot: bot.state), \
                patch.object(self.runner, 'help_up'), \
                patch.object(self.runner, 'wait_for', side_effect=RuntimeError('wait failed')):
            with self.assertRaisesRegex(RuntimeError, 'wait failed'):
                self.runner.help_up_raid((client, host), {})

    def test_consolidated_legacy_suite_keeps_postraid_source_policy(self):
        with patch.object(self.runner, 'postraid_ready', return_value=True):
            self.assertEqual(self.runner.postraid_for('stageEnterPlace'), 'postraid')
        source = inspect.getsource(self.runner.SUITES['stageEnterPlace'])
        self.assertIn('join_both_and_clear(', source)
        self.assertNotIn('# clear_raid(', source)

    def test_all_live_raid_and_new_game_names_stay_out_of_saved_routes(self):
        with patch.object(inspect, 'getsource', side_effect=AssertionError('getsource must not run')):
            for name in sorted(self.runner.LIVE_RAID | self.runner.NEW_GAME):
                with self.subTest(stage=name):
                    self.assertEqual(self.runner.postraid_for(name), '')
        self.ready.assert_not_called()

    def test_warm_routes_before_source_inspection(self):
        with patch.dict(os.environ, {'G3_SANDBOX': '0'}):
            with patch.object(inspect, 'getsource', side_effect=AssertionError('getsource must not run')), \
                    patch.object(self.runner, 'postraid_ready', return_value=True):
                self.assertEqual(self.runner.postraid_for('warm'), 'postraid')
            with patch.object(inspect, 'getsource', side_effect=AssertionError('getsource must not run')), \
                    patch.object(self.runner, 'postraid_ready', return_value=False):
                self.assertEqual(self.runner.postraid_for('warm'), '')

    def test_normal_source_inspection_detects_join_or_clear_helper_calls(self):
        with patch.object(self.runner, 'postraid_ready', return_value=True), \
                patch.object(inspect, 'getsource', return_value='host, client = join_both_and_clear(copies, report)') as getsource:
            self.assertEqual(self.runner.postraid_for('stageEnterPlace'), 'postraid')
        getsource.assert_called_once_with(self.runner.SUITES['stageEnterPlace'])

        with patch.object(self.runner, 'postraid_ready', return_value=True), \
                patch.object(inspect, 'getsource', return_value='host, client = clear_raid(copies, report)'):
            self.assertEqual(self.runner.postraid_for('stageEnterPlace'), 'postraid')

        with patch.object(inspect, 'getsource', return_value='host, client = join_both(copies)'):
            self.assertEqual(self.runner.postraid_for('stageEnterPlace'), '')

    def test_murder_counts_returns_fourth_count_or_none(self):
        with patch.object(self.runner, 'enclave_counts', return_value=[1, 2, 3, 4, 9]):
            self.assertEqual(self.runner.murder_counts(object(), 'Ardea'), 9)
        with patch.object(self.runner, 'enclave_counts', return_value=[]):
            self.assertIsNone(self.runner.murder_counts(object(), 'Ardea'))

    def test_native_shared_spawn_is_verified_before_client_is_offset(self):
        copies = {tag: SimpleNamespace(bot=tag) for tag in ('host', 'client')}
        baseline = {'host': {'pos': (77000, -280, -61000)}, 'client': {'pos': (77800, -280, -61000)}}
        with patch.object(self.runner, 'hero_pos', return_value=baseline['host']['pos']), \
                patch.object(self.runner, 'move_hero') as move, patch.object(self.runner.time, 'sleep'):
            report = {}
            self.runner.arrange_sandbox(copies, report, baseline)
        move.assert_called_once_with('client', baseline['client']['pos'])
        self.assertEqual(report['sandbox_placement'][0]['native_client'], baseline['host']['pos'])

    def test_client_in_wrong_native_world_is_not_silently_teleported(self):
        copies = {tag: SimpleNamespace(bot=tag) for tag in ('host', 'client')}
        baseline = {'host': {'pos': (77000, -280, -61000)}, 'client': {'pos': (77800, -280, -61000)}}
        with patch.object(self.runner, 'hero_pos', side_effect=[baseline['host']['pos'], (0, 0, 0)]), \
                patch.object(self.runner, 'move_hero') as move:
            with self.assertRaisesRegex(AssertionError, 'Client did not load'):
                self.runner.arrange_sandbox(copies, {}, baseline)
        move.assert_not_called()

    def test_host_in_wrong_native_world_is_refused(self):
        copies = {tag: SimpleNamespace(bot=tag) for tag in ('host', 'client')}
        baseline = {'host': {'pos': (77000, -280, -61000)}, 'client': {'pos': (77800, -280, -61000)}}
        with patch.object(self.runner, 'hero_pos', return_value=(0, 0, 0)), patch.object(self.runner, 'move_hero') as move:
            with self.assertRaisesRegex(AssertionError, 'Host did not load'):
                self.runner.arrange_sandbox(copies, {}, baseline)
        move.assert_not_called()

    def test_bot_queue_preserves_sequence_and_log_offset(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root/'qa-engine.log').write_text('previous log')
            bot = self.runner.Bot(None, root, 0, root=root)
            self.assertEqual(bot.queue('closewindow'), len('previous log'))
            self.assertEqual((root/'qa-command.txt').read_text(), '2 closewindow\n')
            bot.queue('state')
            self.assertEqual((root/'qa-command.txt').read_text(), '3 state\n')

    def test_regular_bot_send_still_waits_for_acknowledgement(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            bot = self.runner.Bot(None, root, 0, root=root)
            with patch.object(bot, 'wait', return_value=True) as wait:
                self.assertEqual(bot.send('state', 9), 0)
            self.assertEqual(wait.call_args.args[1], 9)
            self.assertTrue(wait.call_args.args[0]('COMMAND sequence=2 action=state thread=1'))

    def test_both_closes_are_queued_before_waiting_for_either_renderer(self):
        events = []
        copies = {}
        for tag in ('host', 'client'):
            process = SimpleNamespace(returncode=None)
            process.poll = lambda: None
            process.wait = lambda timeout, tag=tag: events.append('wait '+tag)
            bot = SimpleNamespace(queue=lambda action, tag=tag: events.append('queue '+tag+' '+action))
            copies[tag] = SimpleNamespace(process=process, bot=bot)
        self.assertEqual(self.runner.close_all(copies), {'host': True, 'client': True})
        self.assertEqual(events, ['queue host closewindow', 'queue client closewindow', 'wait host', 'wait client'])

    def test_shutdown_requires_both_lifecycle_markers_without_pup_logging(self):
        with tempfile.TemporaryDirectory() as temp:
            copy = SimpleNamespace(root=Path(temp), report={}, tag='host',
                                   process=SimpleNamespace(returncode=1),
                                   mod_log=lambda: 'pre-shutdown: module unregistered=1',
                                   bot=SimpleNamespace(log=lambda: 'DESTROY window close accepted; detach QA callbacks'))
            self.assertTrue(self.runner.clean_shutdown(copy, True))
            self.assertFalse(self.runner.clean_shutdown(copy, False))
            copy.process.returncode = -1073741819
            self.assertFalse(self.runner.clean_shutdown(copy, True))
            copy.process.returncode = 1
            copy.mod_log = lambda: ''
            self.assertFalse(self.runner.clean_shutdown(copy, True))

    def test_independent_runner_cannot_hide_a_failed_protected_save_check(self):
        report = {'status': 'passed', 'protected_saves_unchanged': False}
        result = SimpleNamespace(stdout=json.dumps(report)+'\nRESULT: FAIL\n', stderr='', returncode=1)
        with patch.object(self.session.subprocess, 'run', return_value=result):
            status, error = self.session.run_alone('stageW2')
        self.assertEqual(status, 'failed')
        self.assertIn('protected saves', error)

    def test_independent_runner_surfaces_missing_fixture_error(self):
        result = SimpleNamespace(stdout='', stderr='RuntimeError: Sandbox fixture missing\n', returncode=1)
        with patch.object(self.session.subprocess, 'run', return_value=result):
            status, error = self.session.run_alone('stageW2')
        self.assertEqual(status, 'failed')
        self.assertIn('Sandbox fixture missing', error)

    def test_dispatcher_returns_nonzero_when_independent_stage_fails(self):
        kernel = SimpleNamespace(ReleaseMutex=lambda handle: None, CloseHandle=lambda handle: None)
        with patch.object(self.session.sys, 'argv', ['session.py', 'stageW2']), \
                patch.object(self.session, 'prepare', return_value=(kernel, 1, Path(self.temp.name), {}, 'test-hash')), \
                patch.object(self.runner, 'snapshot', return_value={}), \
                patch.object(self.session, 'run_alone', return_value=('failed', 'test failure')), patch('builtins.print'):
            with self.assertRaises(SystemExit) as result:
                self.session.main()
        self.assertEqual(result.exception.code, 1)


if __name__ == '__main__':
    unittest.main()
