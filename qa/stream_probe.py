"""Native feasibility gate: load a distant NPC region without moving the real camera."""
import os
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import together as T


def fields(line):
    return dict(re.findall(r'(\w+)=([^\s]+)', line))


def stage_stream_probe(copies, report, negative=False):
    host = copies['host'].launch()
    T.enter_world(host)
    T.clear_raid(host, report)
    candidates = [row for row in T.far_rows(host.bot) if 600 < row['distance'] < 1500]
    assert candidates, 'No distant NPC region is available for the probe'
    target = candidates[0]
    report['target'] = target
    baseline = T.report_line(host.bot, 'worldprocess', 'WORLDPROCESS ')
    report['before'] = baseline
    camera_before = tuple(map(float, fields(baseline)['camera'].split(',')))
    assert target['distance'] * 100 > float(fields(baseline)['camera_far']), 'Target is not outside the original prefetch range'
    if negative:
        raise AssertionError('The positive native streaming probe has no simulated success mode')
    command = 'streamprobe ' + '_'.join(f'{value:.0f}' for value in target['pos']) + '_5000'
    report['probe_start'] = T.report_line(host.bot, command, ('STREAMPROBE ', 'REFUSED '))
    assert report['probe_start'].startswith('STREAMPROBE active=1 '), report['probe_start']
    try:
        samples = report['samples'] = []
        deadline = time.monotonic() + 40
        while time.monotonic() < deadline:
            line = T.report_line(host.bot, 'streamprobe report', ('STREAMPROBE ', 'REFUSED '))
            samples.append(line)
            state = fields(line)
            assert state.get('failed') == '0', line
            if all(int(state.get(key, '0')) > 0 for key in ('near_npcs', 'cells', 'nav_npcs', 'floor_npcs')):
                break
            time.sleep(2)
        report['samples'] = samples
        assert int(fields(samples[-1]).get('near_npcs', '0')) > 0, 'No distant NPC was processed by the host'
        assert int(fields(samples[-1]).get('nav_npcs', '0')) > 0, 'Distant NPCs have no navigation zone'
        assert int(fields(samples[-1]).get('floor_npcs', '0')) > 0, 'Distant NPCs have no collision floor'
        after = T.report_line(host.bot, 'worldprocess', 'WORLDPROCESS ')
        report['after'] = after
        camera_after = tuple(map(float, fields(after)['camera'].split(',')))
        assert T.distance(camera_after, camera_before) < 100, 'The remote probe moved the real camera'
        assert fields(after)['camera_far'] == fields(baseline)['camera_far'], 'The probe changed the real camera far clip'
    finally:
        report['stop'] = T.report_line(host.bot, 'streamprobe off', 'STREAMPROBE ')


def stage_passive_probe(copies, report, negative=False):
    host = copies['host'].launch()
    report['prepare'] = T.report_line(host.bot, 'passiveprobe prepare', ('PASSIVEPROBE ', 'REFUSED '))
    assert 'prepared=1' in report['prepare'], report['prepare']
    T.enter_world(host)
    T.clear_raid(host, report)
    T.report_line(host.bot, 'tutorialoff', 'TUTORIALOFF ')
    T.report_line(host.bot, 'unarm', 'UNARM ')
    nearby = T.npcs(host.bot)
    humans = [(guid, npc) for name in ('Hamlar', 'Gorn', 'Slave_03', 'Slave_04', 'Diego', 'Milten', 'Lester')
              for guid, npc in nearby.items() if npc['name'] == name and not npc['dead']]
    assert humans, 'The passive replay probe needs a nearby humanoid NPC'
    guid, source = humans[0]
    target_name = 'G3T_ReplayProbe'
    report['clone'] = T.report_line(host.bot, 'passiveprobe clone_' + source['name'], ('PASSIVEPROBE ', 'REFUSED '))
    assert 'cloned=1' in report['clone'], report['clone']
    time.sleep(2)
    report['source'], report['target'] = source['name'], target_name
    goal = (source['pos'][0] + 1200, source['pos'][1], source['pos'][2])
    report['walk'] = T.report_line(host.bot, 'npcwalk ' + guid + '_' + '_'.join(f'{v:.0f}' for v in goal), 'NPCWALK ')
    time.sleep(2)
    prefix = 'pose_' if os.environ.get('G3_EXACT_POSE', '1') != '0' else ''
    report['probe_start'] = T.report_line(host.bot, f"passiveprobe {prefix}{source['name']}-{target_name}",
                                        ('PASSIVEPROBE ', 'REFUSED '))
    assert fields(report['probe_start']).get('active') == '1', report['probe_start']
    try:
        at = T.npcs(host.bot)[guid]['pos']
        T.move_hero(host.bot, (at[0] + 250, at[1] + 30, at[2] - 1200))
        report['view'] = T.report_line(host.bot, 'face ' + guid, 'FACE ')
        samples = report['samples'] = []
        for _ in range(5):
            time.sleep(0.5)
            samples.append(T.report_line(host.bot, 'passiveprobe report', 'PASSIVEPROBE '))
        last = fields(samples[-1])
        T.snap(host, 'passive-skeleton-replay', report)
        assert last['failed'] == '0', samples[-1]
        assert int(last['frames']) > 30 and int(last['slot_checks']) > 30, samples[-1]
        assert int(last['entity_blocked']) + int(last['properties_blocked']) > 0, samples[-1]
        assert int(last['slot_errors']) == 0, samples[-1]
        assert float(last['max_position_error']) <= 1, samples[-1]
        assert float(last['max_rotation_error']) <= 0.1, samples[-1]
        assert float(last['max_phase_error']) <= 0.001, samples[-1]
        assert int(last['bone_checks']) > 30, samples[-1]
        assert float(last['max_bone_error']) <= 5, samples[-1]
        assert float(last['immediate_bone_error']) <= 5, samples[-1]
    finally:
        report['probe_stop'] = T.report_line(host.bot, 'passiveprobe off', 'PASSIVEPROBE ')


def stage_host_render_probe(copies, report, negative=False):
    """Keep the real host camera and terrain intact while the remote player's streaming region changes."""
    assert not negative, 'This probe requires the actual host-rendering path'
    host, client = T.join_both(copies)
    T.clear_raid(host, report)
    for copy in (host, client):
        T.report_line(copy.bot, 'tutorialoff', 'TUTORIALOFF ')
        T.report_line(copy.bot, 'unarm', 'UNARM ')
    T.report_line(host.bot, 'console god', 'CONSOLE ')

    def observe(label):
        start = host.bot.send('worldprocess')
        process = host.bot.line('WORLDPROCESS ', 10, 'host camera state', start)
        render = host.bot.line('WORLDRENDER ', 10, 'host render state', start)
        T.snap(host, label, report)
        return {'process': fields(process), 'render': fields(render)}

    try:
        time.sleep(3)
        home = T.hero_pos(host.bot)
        before = report['before'] = observe('host-before-remote')
        assert int(before['render']['statics']) > 0, 'No host static-world baseline'
        report['remote_jump'] = T.teleport_to(client.bot, 'Kent')
        time.sleep(10)
        far = report['far'] = observe('host-with-remote-region')
        assert int(far['render']['remote_regions']) > 0, 'No remote streaming region was exercised'
        guid, _ = T.npc_named(client.bot, 'Kent')
        assert guid, 'The remote destination NPC is unavailable'
        report['remote_npc'] = T.report_line(client.bot, 'npclife ' + guid, 'NPCLIFE ')
        assert fields(report['remote_npc'])['ready'] == '1', 'Remote NPC replay is not ready'
        T.snap(client, 'remote-npcs', report)
        report['remote_return'] = T.move_hero(client.bot, (home[0] + 200, home[1], home[2]))
        time.sleep(10)
        back = report['back'] = observe('host-after-remote-return')
        T.snap(client, 'npcs-after-return', report)
        assert int(back['render']['remote_regions']) == 0, 'Remote camera did not retire after the client returned'
        for sample in (far, back):
            assert sample['render']['main_database'] == before['render']['main_database'], 'The main camera database changed'
            assert sample['process']['camera_far'] == before['process']['camera_far'], 'The main camera far clip changed'
            assert T.distance(tuple(map(float, sample['process']['camera'].split(','))),
                              tuple(map(float, before['process']['camera'].split(',')))) < 100, 'The main camera moved'
            assert int(sample['render']['statics']) >= 0.9 * int(before['render']['statics']), \
                'Remote streaming removed host static-world cache entries: ' + str(sample)
        report['npc_closeup'] = T.teleport_to(client.bot, 'Hamlar')
        guid, _ = T.npc_named(client.bot, 'Hamlar')
        T.report_line(client.bot, 'aimat ' + guid, 'TARGET ')
        time.sleep(1)
        T.snap(client, 'canonical-npc-closeup', report)
        report['visual_review_required'] = True
    finally:
        T.report_line(host.bot, 'console god', 'CONSOLE ')


def stage_readiness_probe(copies, report, negative=False):
    """Keep the original nearby Diego scene usable through native secondary-slot and facial changes."""
    assert not negative, 'This probe needs the production canonical-pose readiness gate'
    host, client = T.join_both(copies)
    T.clear_raid(host, report)
    for copy in (host, client):
        T.report_line(copy.bot, 'tutorialoff', 'TUTORIALOFF ')
        T.report_line(copy.bot, 'unarm', 'UNARM ')
        T.teleport_to(copy.bot, 'Diego')
    time.sleep(5)
    guid, _ = T.npc_named(client.bot, 'Diego')
    assert guid, 'The nearby Diego mirror is missing'
    before = fields(T.report_line(client.bot, 'passivestats', 'PASSIVE '))
    samples = report['samples'] = []
    for _ in range(12):
        life = fields(T.report_line(client.bot, 'npclife ' + guid, 'NPCLIFE '))
        passive = fields(T.report_line(client.bot, 'passivestats', 'PASSIVE '))
        samples.append({'life': life, 'passive': passive})
        assert passive['readiness_warnings'] == before['readiness_warnings'], 'A nearby NPC lost presentation persistently'
        time.sleep(3)
    assert sum(row['life']['ready'] == '1' for row in samples) >= 10, 'Canonical nearby replay is intermittently unusable'
    assert int(samples[-1]['passive']['posed_frames']) - int(before['posed_frames']) > 100, 'No new canonical poses were shown'
    T.snap(client, 'nearby-diego-readiness', report)


if __name__ == '__main__':
    T.SUITES['streamProbe'] = stage_stream_probe
    T.SUITES['passiveProbe'] = stage_passive_probe
    T.SUITES['hostRenderProbe'] = stage_host_render_probe
    T.SUITES['readinessProbe'] = stage_readiness_probe
    T.main()
