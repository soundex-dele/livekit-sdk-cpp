"""Policy logic only: fixture thresholds are not deployment recommendations."""
import unittest

from studio_capacity_limits import SHAPES, evaluate


class CapacityGate(unittest.TestCase):
    def setUp(self):
        self.policy = {'minSeconds': 10, 'processesPerShape': 2,
                       'shapes': {s: {'minPreviewFps': 29, 'maxClockSkipped': 1} for s in SHAPES}}
        self.report = {'passed': True, 'secondsPerSession': 10, 'renderer': 'D3D11 WARP',
                       'processesPerShape': 2, 'shapes': []}
        for shape in sorted(SHAPES):
            w, h = map(int, shape.split('x'))
            self.report['shapes'].append({'width': w, 'height': h, 'errors': [], 'sessions': [
                {'directory': f'{shape}-{i}', 'runUs': 10000000, 'returnCode': 0,
                 'programAudio': {'activeSkippedSamples': 0},
                 'measuredPreviewFps': 29, 'clockSkipped': 1} for i in range(2)]})

    def test_worst_session_not_average(self):
        self.assertTrue(all(x['passed'] for x in evaluate(self.report, self.policy)))
        self.report['shapes'][-1]['sessions'][1]['clockSkipped'] = 2
        missed = [x for x in evaluate(self.report, self.policy) if not x['passed']]
        self.assertEqual([(x['metric'], x['actual']) for x in missed], [('maxClockSkipped', 2)])

    def test_no_partial_or_wrong_workload(self):
        for field, value in [('passed', False), ('experimentalPoll', True), ('secondsPerSession', 3), ('processesPerShape', 1)]:
            with self.assertRaises(ValueError):
                evaluate(dict(self.report, **{field: value}), self.policy)
        with self.assertRaises(ValueError):
            evaluate(dict(self.report, shapes=self.report['shapes'][:-1]), self.policy)

    def test_reject_bad_policy(self):
        for limits in [{}, {'typo': 10}, {'minPreviewFps': False}, {'maxClockSkipped': float('nan')}]:
            self.policy['shapes']['1280x720'] = limits
            with self.assertRaises(ValueError):
                evaluate(self.report, self.policy)


if __name__ == '__main__':
    unittest.main()
