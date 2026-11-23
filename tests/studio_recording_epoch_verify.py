"""Recording starts at media readiness, not at the request to open a file."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from studio_capacity_verify import inspect
from studio_pcm_timing import load_rows


def verify(exe, ffmpeg, base):
    base.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix='open-', dir=base))
    report = {'passed': False, 'cases': {}}
    try:
        for hot in (False, True):
            case = run/('hot' if hot else 'cold')
            case.mkdir()
            subprocess.run([exe, case, '1280', '720', '2', '--pcm-trace',
                            '--slow-hot-open' if hot else '--slow-open'], check=True, timeout=30)
            m = json.loads((case/'metrics.json').read_bytes())
            start, end = m['recordOpenHoldNs']
            request, epoch = m['recordRequestNs'], m['recordEpochNs']
            if not (request <= start < end <= epoch and end-start >= 180_000_000):
                raise ValueError(f'{case.name}: recording epoch includes preparation')
            rows = load_rows(case/'pcm-write.csv', 'slot,mixed,enter,leave,accepted')
            if any(b[0]-a[0] != 10_000_000 for a, b in zip(rows, rows[1:])):
                raise ValueError(f'{case.name}: non-contiguous audio clock')
            during = [r for r in rows if start <= r[2] <= r[3] <= end]
            if (hot and (not during or not all(r[4] for r in during))) or (not hot and during):
                raise ValueError(f'{case.name}: unexpected live audio during preparation')
            recorded = [r for r in rows if r[0] >= epoch]
            if not recorded or (m['firstRecordFrame'] > 0) != hot:
                raise ValueError(f'{case.name}: missing media boundary')
            inspect(ffmpeg, ffmpeg.with_name('ffprobe.exe'), case, 1280, 720, 2, m,
                    first_frame=m['firstRecordFrame'], record_slots=len(recorded))
            report['cases'][case.name] = {'requestToEpochMs': (epoch-request)/1e6,
                'epochToFirstPcmMs': (recorded[0][0]-epoch)/1e6, 'liveWritesDuringOpen': len(during),
                'firstVideoMarker': m['firstRecordFrame'], 'decodedFrames': m['decodedFrames'],
                'decodedToneWindows': m['decodedToneWindows']}
        report['passed'] = True
    finally:
        (run/'verification.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        print(run, report, flush=True)


if __name__ == '__main__':
    verify(*map(Path, sys.argv[1:]))
