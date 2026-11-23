"""Exercise the Windows software A/V path; optional limits apply only to this profile."""
import argparse
import hashlib
import json
import platform
from pathlib import Path
import subprocess
import tempfile

PROFILE = 'windows-software-av-720p25-v1'
# Keep node IDs (e.g. 'file.record') intact when resolving JSON paths.
METRICS = {'.'.join(path): path for path in (
    ('record', 'scheduleToEncoderReturn', 'p99Us'), ('record', 'stopAndFinalizeUs'),
    ('record', 'nodes', 'file.record', 'queueWait', 'p99Us'),
    ('playback', 'videoLate', 'p99Us'), ('playback', 'audioLate', 'p99Us'), ('decode', 'decodeUs'))}


def digest(path):
    with path.open('rb') as f:
        h = hashlib.sha256()
        for data in iter(lambda: f.read(1024 * 1024), b''):
            h.update(data)
        return h.hexdigest()


def check_limits(doc, policy):
    if not isinstance(policy, dict) or set(policy) != {'profile', 'minFrames', 'max'}:
        raise ValueError('expected profile, minFrames, max')
    if policy['profile'] != PROFILE or doc['profile'] != PROFILE:
        raise ValueError('profile mismatch')
    if type(policy['minFrames']) is not int or not 100 <= policy['minFrames'] <= doc['sourceFrames']:
        raise ValueError('not enough frames for the requested policy (minimum 100)')
    limits = policy['max']
    if not isinstance(limits, dict) or not limits or set(limits) - set(METRICS):
        raise ValueError(f'max must contain supported metrics: {METRICS}')
    checks = {}
    for name, limit in limits.items():
        actual = doc
        for part in METRICS[name]:
            actual = actual[part]
        if any(type(i) is not int or i < 0 for i in (actual, limit)):
            raise ValueError(f'{name}: expected non-negative integers in microseconds')
        checks[name] = {'actualUs': actual, 'maxUs': limit, 'passed': actual <= limit}
    return checks


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('exe', type=Path)
    p.add_argument('out', type=Path)
    p.add_argument('--frames', type=int, default=150, choices=range(25, 1501), metavar='25..1500')
    p.add_argument('--budgets', type=Path)
    args = p.parse_args()
    exe = args.exe.resolve(strict=True)
    args.out.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix='profile-', dir=args.out.resolve()))
    print('Media evidence:', run, flush=True)
    info = {'profile': PROFILE, 'frames': args.frames, 'os': platform.platform(),
            'cpu': platform.processor(), 'executableSha256': digest(exe),
            'mode': 'explicit-budgets' if args.budgets else 'functional-only', 'passed': False}
    try:
        if args.budgets:
            info['policy'] = json.loads(args.budgets.read_bytes())
        with (run / 'run.log').open('wb') as log:
            subprocess.run([str(exe), '--profile', str(run), str(args.frames)], check=True,
                           stdout=log, stderr=subprocess.STDOUT, timeout=90 + args.frames / 10)
        doc = json.loads((run / 'media-metrics.json').read_bytes())
        if doc['profile'] != PROFILE or any(n != args.frames for n in
                (doc['sourceFrames'], doc['record']['recorded'], doc['decode']['videoFrames'],
                 doc['playback']['videoLate']['samples'])):
            raise ValueError('incomplete media path')
        info['mp4Sha256'] = digest(run / 'profile.mp4')
        if args.budgets:
            checks = check_limits(doc, info['policy'])
            info['checks'] = checks
            if not all(c['passed'] for c in checks.values()):
                raise ValueError(f'media budget exceeded: {checks}')
        info['passed'] = True
    finally:
        (run / 'verification.json').write_text(json.dumps(info, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
