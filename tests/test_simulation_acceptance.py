import unittest

from simulation_acceptance import simulation_rows
from uav_requirements import REQUIREMENTS


class SimulationVerdicts(unittest.TestCase):
    def rows(self, scenarios=(), recovery=(), gaps=None):
        return {r['id']: r for r in simulation_rows(
            [{'id': rid, 'title': values[0], 'evidence': []} for rid, values in REQUIREMENTS.items()],
            list(scenarios), list(recovery), gaps or {})}

    def scene(self, protocol='udp', kind='video', codec='h264', ok=True):
        ids = ['two_sources_two_viewers', 'independent_unsubscribe_resubscribe',
               'unpublish_isolation', 'republish_new_sid', 'selected_ice_protocol']
        if kind == 'dynacast':
            ids = ['aggregate_demand_stops_source_and_rtp', 'selected_ice_protocol']
        return {'protocol': protocol, 'scenario': kind, 'codec': codec, 'ok': ok,
                'folder': f'{protocol}-{kind}-{codec}', 'checks': [{'id': c, 'status': 'PASS'} for c in ids]}

    def test_missing_evidence_cannot_become_simulation_pass(self):
        rows = self.rows()
        self.assertEqual(len(rows), 34)
        self.assertFalse(any(r['status'] == '通过' for r in rows.values()))

    def test_device_absence_does_not_block_proven_multi_viewer_or_dynacast(self):
        scenes = [self.scene(kind=k, codec=c) for k in ('video', 'dynacast') for c in ('h264', 'h265')]
        rows = self.rows(scenes)
        self.assertEqual(rows['V-02']['status'], '通过')
        self.assertEqual(rows['V-08']['status'], '通过')
        self.assertEqual(rows['N-01']['status'], '未测')
        self.assertEqual(rows['P-10']['status'], '未测')

    def test_missing_assertion_does_not_pass_even_with_ok_true(self):
        scenes = [self.scene(codec=c) for c in ('h264', 'h265')]
        scenes[1]['checks'] = []
        self.assertNotEqual(self.rows(scenes)['V-02']['status'], '通过')

    def test_failed_repeat_cannot_be_erased(self):
        scenes = [self.scene(protocol='tcp', codec=c) for c in ('h264', 'h265')]
        self.assertEqual(self.rows(scenes)['B-04']['status'], '通过')
        scenes.append(self.scene(protocol='tcp', ok=False))
        self.assertNotEqual(self.rows(scenes)['B-04']['status'], '通过')

    def test_recovery_exit_code_without_verified_artifacts_is_not_enough(self):
        evidence = [{'transport': p, 'scenario': 'restart', 'exit_code': 0, 'log': p} for p in ('udp', 'tcp')]
        self.assertNotEqual(self.rows(recovery=evidence)['N-05']['status'], '通过')
        for r in evidence:
            r['verified'] = True
        self.assertEqual(self.rows(recovery=evidence)['N-05']['status'], '通过')

    def test_known_failure_wins_over_simulation_success(self):
        scenes = [self.scene(kind='dynacast', codec=c) for c in ('h264', 'h265')]
        row = self.rows(scenes, gaps={'V-08': 'regression failed'})['V-08']
        self.assertEqual(row['status'], '未通过')

    def test_partial_and_unrun_are_distinct(self):
        rows = simulation_rows([{'id': 'Q-02', 'evidence': ['component.log']},
                                {'id': 'N-01', 'evidence': []}], [], [], {})
        self.assertEqual(rows[0]['coverage'], '部分通过，待补模拟')
        self.assertEqual(rows[1]['coverage'], '尚未模拟')


if __name__ == '__main__':
    unittest.main()
