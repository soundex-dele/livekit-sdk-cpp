"""Correlate one Studio run's monotonic timestamp trace; no cross-process clock joins."""
from bisect import bisect_left, bisect_right
from collections import Counter
import csv
from math import ceil

MS = 1_000_000


def load_rows(path, header):
    with path.open(newline='', encoding='ascii') as f:
        rows = csv.reader(f)
        if next(rows, []) != header.split(','):
            raise ValueError(f'{path.name}: incorrect trace columns')
        result = [tuple(map(int, row)) for row in rows]
    if any(len(row) != len(header.split(',')) for row in result):
        raise ValueError(f'{path.name}: incomplete trace')
    return result


def ordered_spans(spans):
    if any(a > b or (i and spans[i-1][1] > a) for i, (a, b) in enumerate(spans)):
        raise ValueError('negative or overlapping sequential intervals')
    return [a for a, b in spans]


def quantiles(values):
    values = sorted(values)
    return {f'p{p}Us': round(values[ceil(p*len(values)/100)-1]/1000, 1) for p in (50, 95, 99)} if values else {}


def correlate(writes, reads, videos, steps, end):
    slots = [w[0] for w in writes]
    if any(a >= b for a, b in zip(slots, slots[1:])) or any(
            not (c <= m <= a <= b and ok in (0, 1)) for c, m, a, b, ok in writes):
        raise ValueError('invalid PCM producer timeline')
    if any(r[3] not in (0, 1) for r in reads) or any(a[0] >= b[0] for a,b in zip(reads,reads[1:])):
        raise ValueError('invalid PCM reader timeline')
    ordered_spans([(w[2], w[3]) for w in writes])
    ordered_spans([(r[1], r[2]) for r in reads])
    vstarts, sstarts = ordered_spans(videos), ordered_spans(steps)

    def contains(starts, spans, enter, leave):
        i = bisect_right(starts, enter)-1
        return i >= 0 and leave <= spans[i][1]

    def reader_location(enter, leave):
        if contains(vstarts, videos, enter, leave):
            return 'inSynchronousVideo'
        if contains(sstarts, steps, enter, leave):
            return 'inOtherOrMixedStepWork'
        i = bisect_right(sstarts, enter)-1
        if i >= 0 and steps[i][1] <= enter and i+1 < len(steps) and leave <= steps[i+1][0]:
            return 'betweenStudioSteps'
        return 'boundaryOrOutside'

    reasons, late_location = Counter(), Counter()
    late_unmixed = 0
    reads_active, slack = [], []
    for slot, enter, leave, silence in reads:
        if leave > end:  # Never attribute silence emitted during drain/release to active playback.
            continue
        reads_active.append((slot, enter, leave))
        # Same asymmetric half-open matching window as PcmSourceReader::read().
        lo = bisect_left(slots, slot-5*MS)
        hi = bisect_left(slots, slot+5*MS)
        if hi-lo != 1:
            if silence:
                reasons['noMatchingWrite' if hi==lo else 'ambiguousWrites'] += 1
            elif hi == lo:
                raise ValueError('consumed PCM has no producer timestamp')
            continue
        _, mixed, went, wleft, ok = writes[lo]
        if ok:
            slack.append(enter-wleft)  # Negative means write returned after read began.
        if not silence:
            if not ok or went > leave:
                raise ValueError('impossible consumed PCM ordering')
            continue
        if not ok:
            reasons['writeRejected'] += 1
        elif went > leave:
            reasons['acceptedAfterRead'] += 1
            late_location[reader_location(enter, leave)] += 1
            late_unmixed += mixed > leave
        elif wleft < enter:
            reasons['acceptedBeforeRead'] += 1
        else:
            reasons['overlappingCalls'] += 1
    return {'activeReads': len(reads_active), 'activeSilentReads': sum(reasons.values()),
            'excludedShutdownReads': len(reads)-len(reads_active), 'silenceByReason': dict(reasons),
            'acceptedAfterReadLocation': dict(late_location), 'notYetMixedAtLateRead': late_unmixed,
            'slotToMix': quantiles([m-c for c,m,_,_,_ in writes]),
            'mixToWriteEntry': quantiles([a-m for _,m,a,_,_ in writes]),
            'writeCall': quantiles([b-a for _,_,a,b,_ in writes]),
            'slotToReadExit': quantiles([leave-slot for slot,_,leave in reads_active]),
            'firstReadSlotToExitUs': round((reads_active[0][2]-reads_active[0][0])/1000,1) if reads_active else None,
            'readEntryMinusAcceptedWriteExit': quantiles(slack),
            'videoDispatch': quantiles([b-a for a,b in videos]),
            'betweenSteps': quantiles([a[0]-b[1] for b,a in zip(steps,steps[1:])])}


def inspect_trace(directory, metrics):
    writes = load_rows(directory/'pcm-write.csv', 'slot,mixed,enter,leave,accepted')
    reads = load_rows(directory/'pcm-read.csv', 'slot,enter,leave,silence')
    videos = load_rows(directory/'video-dispatch.csv', 'enter,leave')
    steps = load_rows(directory/'graph-step.csv', 'enter,leave')
    a = metrics['programAudio']
    if (len(steps) != metrics['ticks'] or len(videos) != metrics['previewFrames'] or
            len(writes) != metrics['received']['studio.audio-output'] or
            480*sum(w[4] for w in writes) != a['acceptedSamples'] or
            480*sum(r[3] for r in reads) != metrics['totalSilenceSamples'] or
            480*sum(1-r[3] for r in reads) != a['consumedSamples']):
        raise ValueError('PCM trace does not reconcile with graph and source counters')
    return correlate(writes, reads, videos, steps, metrics['traceRunEndNs'])
