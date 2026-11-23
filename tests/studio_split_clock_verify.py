"""Independently decode cross-mailbox capture ordering, not just packet counts."""
from array import array
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def verify(directory, ffmpeg, mode):
    file = directory / 'take.mp4'
    info = json.loads(subprocess.check_output([ffmpeg.with_name('ffprobe.exe'), '-v', 'error',
        '-show_frames', '-show_entries', 'frame=media_type,best_effort_timestamp_time',
        '-of', 'json', file], timeout=8))
    frames = info['frames']
    def times(kind):
        return [float(f['best_effort_timestamp_time']) for f in frames if f['media_type'] == kind]
    v, a = times('video'), times('audio')
    if len(v) != 2 or not a:
        raise ValueError(f'missing streams or video frames: {v}, {a}')
    prefix = [ffmpeg, '-v', 'error', '-nostdin', '-xerror', '-i', file]
    video = subprocess.check_output(prefix + ['-map', '0:v:0', '-vf',
        'scale=1:1:in_range=full:out_range=full', '-fps_mode', 'passthrough',
        '-pix_fmt', 'gray', '-f', 'rawvideo', 'pipe:1'], timeout=8)
    pcm = array('f')
    pcm.frombytes(subprocess.check_output(prefix + ['-map', '0:a:0', '-ar', '48000',
        '-ac', '2', '-f', 'f32le', 'pipe:1'], timeout=8))
    left, right = pcm[::2], pcm[1::2]
    signal = [i for i, val in enumerate(left) if abs(val) > .12]
    if len(video) != 2 or abs(video[0]-40)>6 or abs(video[1]-210)>6 or not signal:
        raise ValueError(f'bad decoded WARP frame signatures or missing tone: {list(video)}')
    if sum(x*y for x,y in zip(left,right)) >= -10:
        raise ValueError('stereo content lost')
    onset = a[0] + signal[0]/48000 - v[0]
    tone_duration = (signal[-1]-signal[0]+1)/48000
    delta = v[1]-v[0]
    timing = [tuple(map(int, row.split())) for row in (directory/'pcm-timing-us.txt').read_text().splitlines()]
    if len(timing) != 20 or any(len(t) != 2 or t[0] > t[1] for t in timing):
        raise ValueError('incomplete program PCM timing sample')
    expected_onset, expected_delta = (.1, 1/3) if mode == 'hold-video' else (-.1, .2)
    # Compare PTS deltas, not container zero: AAC may add a priming frame.
    # Separate tolerance for AAC onset (one codec frame) and Matroska's ms ticks.
    return {'videoPtsSec': v, 'toneStartMinusFirstVideoSec': onset, 'toneDurationSec': tone_duration,
            'videoIntervalSec': delta, 'decodedLuma': list(video), 'maxPcmWriteLateUs': max(w for _,w in timing),
            'maxSubmitToPcmWriteUs': max(w-s for s,w in timing),
            'submittedAfter10ms': sum(s>10000 for s,_ in timing),
            'pcmWritesAfter10ms': sum(w>10000 for _,w in timing),
            'passed': abs(onset-expected_onset)<.024 and abs(delta-expected_delta)<.003 and
                      abs(tone_duration-.2)<.012}


def main():
    exe, ffmpeg, output = map(Path, sys.argv[1:])
    output.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix='split-', dir=output))
    result = {}
    print('Evidence:', run, flush=True)
    for mode in ['hold-video', 'hold-audio']:
        d = run/mode
        d.mkdir()
        try:
            with (d/'run.log').open('wb') as log:
                subprocess.run([exe, d, mode], check=True, stdout=log, stderr=subprocess.STDOUT, timeout=10)
            result[mode] = verify(d, ffmpeg, mode)
        except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
            result[mode] = {'passed': False, 'error': str(exc)}
    (run/'verification.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result))
    return 0 if all(v['passed'] for v in result.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
