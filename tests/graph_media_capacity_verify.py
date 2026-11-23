"""Measure concurrent recording and playback plus post-close Windows process resources."""
import argparse
import json
import platform
import subprocess
import tempfile
from pathlib import Path

from graph_media_profile_verify import PROFILE, check_limits, digest

COUNTERS = ('privateBytes', 'handles', 'threads')


def inspect(run, frames, sessions, rounds, max_growth, policy=None):
    rows = [json.loads(line) for line in (run / 'resources.jsonl').read_bytes().splitlines()]
    if not rows or len({row['pid'] for row in rows}) != 1:
        raise ValueError('not a complete single-process capture')
    phases = [(row['round'], row['phase']) for row in rows]
    if phases[0] != (-1, 'initial') or any(type(row[key]) != int or row[key] <= 0
                                           for row in rows for key in COUNTERS):
        raise ValueError('invalid process counters')
    pos = 1
    result = []
    failures = []
    for epoch in range(rounds):
        begin = pos
        while pos < len(rows) and phases[pos] == (epoch, 'active'):
            pos += 1
        active = rows[begin:pos]
        if not active or pos >= len(rows) or phases[pos] != (epoch, 'closed'):
            raise ValueError(f'round {epoch}: missing or out-of-order sampling phases')
        after = {k: rows[pos][k] for k in COUNTERS}
        pos += 1
        if not epoch:
            warm = after
        growth = {k: after[k] - warm[k] for k in COUNTERS}
        for k in COUNTERS:
            if growth[k] > max_growth[k]:
                failures.append(f'round {epoch}: post-close {k} grew by {growth[k]} > {max_growth[k]}')
        docs = []
        for i in range(sessions):
            d = run / f'round-{epoch}-session-{i}'
            doc = json.loads((d / 'media-metrics.json').read_bytes())
            rec, dec, play = doc['record'], doc['decode'], doc['playback']
            if (doc['profile'] != PROFILE or doc['sourceFrames'] != frames or rec['recorded'] != frames
                    or dec['videoFrames'] != frames or play['videoLate']['samples'] != frames
                    or dec['stereoSamples'] != play['stereoSamples'] or (d/'profile.mp4').stat().st_size == 0):
                raise ValueError(f'incomplete business path: {d}')
            if policy:
                for name, check in check_limits(doc, policy).items():
                    if not check['passed']:
                        failures.append(f'{d.name}: {name} {check}')
            docs.append(doc)
        overlap = {stage: min(d[stage]['lastMediaTickUs'] for d in docs) - max(d[stage]['firstMediaTickUs'] for d in docs)
                   for stage in ('record', 'playback')}
        if min(overlap.values()) <= 0:
            failures.append(f'round {epoch}: no measured overlap in recording or playback')
        result.append({'round': epoch, 'activeSamples': len(active),
                       'activeSampleMax': {k: max(row[k] for row in active) for k in COUNTERS},
                       'closed': after, 'growthFromWarmClosed': growth, 'commonIntervalUs': overlap, 'intervalBasis': 'encoder-and-video-callbacks',
                       'worstSessionP99Us': {
                           'scheduleToEncoderReturn': max(d['record']['scheduleToEncoderReturn']['p99Us'] for d in docs),
                           'playbackVideoLate': max(d['playback']['videoLate']['p99Us'] for d in docs)},
                       'previewEvictions': sum(d['record']['evicted'] for d in docs),
                       'previewSkipped': sum(d['record']['skipped'] for d in docs),
                       'recordAfterFramePeriod': sum(d['record']['afterFramePeriod'] for d in docs)})
    if pos != len(rows):
        raise ValueError('unexpected extra resource records')
    return {'pid': rows[0]['pid'], 'roundMetrics': result, 'violations': failures}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('exe', type=Path)
    p.add_argument('out', type=Path)
    p.add_argument('--frames', type=int, default=25)
    p.add_argument('--sessions', type=int, default=2)
    p.add_argument('--rounds', type=int, default=3)
    p.add_argument('--max-private-growth-mib', type=int, default=64)
    p.add_argument('--max-handle-growth', type=int, default=8)
    p.add_argument('--max-thread-growth', type=int, default=4)
    p.add_argument('--budgets', type=Path, help='optional per-session media budgets (>=100 frames)')
    args = p.parse_args()
    limits = dict(zip(COUNTERS, (args.max_private_growth_mib * 2**20, args.max_handle_growth, args.max_thread_growth)))
    if (not 25 <= args.frames <= 1500 or not 1 <= args.sessions <= 4 or not 3 <= args.rounds <= 12
            or min(limits.values()) < 0):
        p.error('expected 25..1500 frames, 1..4 sessions, 3..12 rounds and non-negative growth limits')
    exe = args.exe.resolve(strict=True)
    args.out.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix='capacity-', dir=args.out.resolve()))
    print('Media capacity evidence:', run, flush=True)
    info = {'profile': PROFILE, 'framesPerSession': args.frames, 'sessions': args.sessions, 'rounds': args.rounds,
            'os': platform.platform(), 'cpu': platform.processor(), 'executableSha256': digest(exe),
            'maxGrowthFromFirstClosed': limits, 'passed': False}
    try:
        if args.budgets:
            info['mediaBudgets'] = json.loads(args.budgets.read_bytes())
            if not isinstance(info['mediaBudgets'], dict) or not info['mediaBudgets']:
                raise ValueError('invalid explicit media budget')
        with (run/'run.log').open('wb') as log:
            rc = subprocess.run([str(exe), '--capacity', str(run), str(args.frames), str(args.sessions), str(args.rounds)],
                                stdout=log, stderr=subprocess.STDOUT, timeout=30 + args.rounds * (45 + args.frames / 8))
        info['returnCode'] = rc.returncode
        rc.check_returncode()
        info.update(inspect(run, args.frames, args.sessions, args.rounds, limits, info.get('mediaBudgets')))
        if info['violations']:
            raise ValueError(info['violations'])
        info['passed'] = True
    finally:
        (run/'verification.json').write_text(json.dumps(info, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
