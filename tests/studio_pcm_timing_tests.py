"""Timing attribution uses producer/consumer call intervals, not rounded wall times."""
import math
from array import array
import unittest
from studio_capacity_verify import inspect_tone_windows
from studio_pcm_timing import MS, correlate


def ms(rows):
    return [tuple(v*MS for v in row) for row in rows]


class RecordedPcmContracts(unittest.TestCase):
    def test_complete_interior_windows_and_both_channels(self):
        block = array('h')
        for i in range(480):
            v = round(10000*math.sin(2*math.pi*i/96))
            block.extend((v, -v))
        padding = array('h', [0]) * (2*2048)
        samples = padding + block*20 + padding
        self.assertEqual(inspect_tone_windows(samples), 20)
        for index, reason in ((2, 'missing stereo tone'), (19, 'wrong stereo phase')):
            with self.subTest(index=index):
                broken = samples[:]
                start = 2048 + 480*index
                for i in range(start, start+480):
                    broken[2*i+1] = -1 if index == 2 else broken[2*i]
                with self.assertRaisesRegex(ValueError, f'{reason}.*sample {start}'):
                    inspect_tone_windows(broken)


class TimingContracts(unittest.TestCase):
    def test_silence_categories_and_shutdown(self):
        # Nanoseconds expressed below in ms. One write follows a video-blocked
        # read, one was rejected, one was ready, one overlaps and one is absent.
        writes = [(*ms([r[:4]])[0], r[4]) for r in [(0, 4, 5, 6, 1), (10, 11, 12, 13, 0),
                  (20, 20, 21, 22, 1), (30, 30, 31, 34, 1)]]
        reads = [(*ms([r])[0], 1) for r in [(0, 2, 3), (10, 12, 13), (20, 24, 25),
                                             (30, 32, 33), (40, 41, 42), (50, 51, 52)]]
        m = correlate(writes, reads, ms([(1, 3)]), ms([(0, 3), (4, 8)]), 45*MS)
        self.assertEqual(m['silenceByReason'], {k: 1 for k in ['acceptedAfterRead', 'writeRejected',
                         'acceptedBeforeRead', 'overlappingCalls', 'noMatchingWrite']})
        self.assertEqual(m['acceptedAfterReadLocation'], {'inSynchronousVideo': 1})
        self.assertEqual((m['notYetMixedAtLateRead'], m['excludedShutdownReads']), (1, 1))

    def test_half_open_slot_and_gap(self):
        writes = [(5*MS, 10*MS, 11*MS, 12*MS, 1)]
        reads = [(0, 2*MS, 3*MS, 1), (10*MS, 7*MS, 8*MS, 1)]
        m = correlate(writes, reads, [], ms([(0, 6), (9, 12)]), 15*MS)
        self.assertEqual(m['silenceByReason'], {'noMatchingWrite': 1, 'acceptedAfterRead': 1})
        self.assertEqual(m['acceptedAfterReadLocation'], {'betweenStudioSteps': 1})

    def test_undecidable_match_not_claimed(self):
        writes = [(0, 0, 1, 2, 1), (4*MS, 4*MS, 5*MS, 6*MS, 1)]
        m = correlate(writes, [(0, 10, 11, 1)], [], [], MS)
        self.assertEqual(m['silenceByReason'], {'ambiguousWrites': 1})
        with self.assertRaises(ValueError):
            correlate([], [(0, 0, 1, 0)], [], [], MS)


if __name__ == '__main__':
    unittest.main()
