"""Hold the production video owner; verify live and recorded PCM, not a model of the graph."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from studio_capacity_verify import inspect
from studio_pcm_timing import inspect_trace, load_rows


def verify(exe, ffmpeg, out):
    out.mkdir(exist_ok=True, parents=True)
    run = Path(tempfile.mkdtemp(prefix='held-', dir=out))
    print('Independent clock evidence:', run, flush=True)
    subprocess.run([exe, run, '1280', '720', '2', '--pcm-trace', '--hold-video'], check=True, timeout=30)
    m = json.loads((run/'metrics.json').read_bytes())
    inspect(ffmpeg, ffmpeg.with_name('ffprobe.exe'), run, 1280, 720, 2, m)
    inspect_trace(run, m)
    start, end = m['videoOwnerHoldNs']
    slots = load_rows(run/'pcm-write.csv', 'slot,mixed,enter,leave,accepted')
    reads = load_rows(run/'pcm-read.csv', 'slot,enter,leave,silence')
    inside = [w for w in slots if start <= w[2] and w[3] <= end]
    played = [r for r in reads if start <= r[1] and r[2] <= end]
    if (end-start < 290_000_000 or len(inside) < 24 or not all(w[4] for w in inside) or
            len(played) < 24 or any(r[3] for r in played) or
            any(b[0]-a[0] != 10_000_000 for a, b in zip(slots, slots[1:]))):
        raise ValueError('missing, rejected or silent PCM across video-owner stall')
    windows = m['decodedToneWindows']  # Same whole-track decoder as the four-shape capacity gate.
    if windows < 150:
        raise ValueError('decoded track too short')
    summary = {'holdMs': (end-start)/1e6, 'writesDuringHold': len(inside), 'readsDuringHold': len(played),
               'silentReadsDuringHold': 0, 'decodedToneWindows': windows}
    (run/'verification.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(summary)


if __name__ == '__main__':
    verify(*map(Path, sys.argv[1:]))
