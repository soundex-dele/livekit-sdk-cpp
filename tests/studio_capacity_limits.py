"""Apply product-owned limits to a completed Windows Studio capacity run, per session."""
import argparse
import hashlib
import json
import math
from pathlib import Path

SHAPES = {'1280x720', '720x1280', '1920x1080', '1080x1920'}
# No default SLA. Field names carry their units; do not average away a failed session.
METRICS = {'minPreviewFps': ('measuredPreviewFps',), 'maxClockSkipped': ('clockSkipped',),
           'maxVideoTickP99Us': ('videoTickP99Us',), 'maxGraphStopUs': ('graphStopUs',),
           'maxProgramVideoRejected': ('programVideo', 'rejected'),
           'maxProgramAudioRejected': ('programAudio', 'rejectedWrites'),
           'maxProgramAudioSilenceSamples': ('programAudio', 'activeSilenceSamples'),
           'maxProgramAudioSkippedSamples': ('programAudio', 'activeSkippedSamples')}


def evaluate(report, policy):
    if (not isinstance(policy, dict) or set(policy) != {'minSeconds', 'processesPerShape', 'shapes'} or
            type(policy['minSeconds']) is not int or not 2 <= policy['minSeconds'] <= 180 or
            type(policy['processesPerShape']) is not int or not 1 <= policy['processesPerShape'] <= 4 or
            not isinstance(policy['shapes'], dict) or set(policy['shapes']) != SHAPES):
        raise ValueError('expected minSeconds, processesPerShape and limits for all four shapes')
    sessions = policy['processesPerShape']
    if (report['passed'] is not True or report.get('experimentalPoll', False) or report['secondsPerSession'] < policy['minSeconds'] or
            report['processesPerShape'] != sessions or report['renderer'] != 'D3D11 WARP'):
        raise ValueError('functional gate, workload or duration does not match the policy')
    checks, seen = [], set()
    for shape in report['shapes']:
        key = f"{shape['width']}x{shape['height']}"
        if key in seen or key not in SHAPES or shape['errors'] or len(shape['sessions']) != sessions:
            raise ValueError('duplicate, failed or incomplete shape')
        seen.add(key)
        limits = policy['shapes'][key]
        if not isinstance(limits, dict) or not limits or set(limits) - METRICS.keys():
            raise ValueError(f'{key}: empty limits or unknown metric')
        for run in shape['sessions']:
            if ('activeSkippedSamples' not in run['programAudio'] or run['returnCode'] != 0 or
                    run['runUs'] < policy['minSeconds'] * 1_000_000):
                raise ValueError(f'{key}: incomplete run')
            for name, limit in limits.items():
                actual = run
                for field in METRICS[name]:
                    actual = actual[field]
                if any(type(v) not in (float, int) or not math.isfinite(v) or v < 0 for v in (actual, limit)):
                    raise ValueError(f'{key}/{name}: expected finite nonnegative numbers')
                passed = actual >= limit if name.startswith('min') else actual <= limit
                checks.append({'shape': key, 'session': run['directory'], 'metric': name,
                               'actual': actual, 'limit': limit, 'passed': passed})
    if seen != SHAPES:
        raise ValueError('missing shape')
    return checks


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('report', type=Path)
    p.add_argument('policy', type=Path)
    args = p.parse_args()
    report, policy = args.report.read_bytes(), args.policy.read_bytes()
    checks = evaluate(json.loads(report), json.loads(policy))
    passed = all(x['passed'] for x in checks)
    print(json.dumps({'passed': passed, 'reportSha256': hashlib.sha256(report).hexdigest(),
                      'policySha256': hashlib.sha256(policy).hexdigest(), 'checks': checks}, indent=2))
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
