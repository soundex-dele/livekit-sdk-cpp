"""Measure real Windows Studio graph sessions (WARP/remote fixture); no devices or network."""
import argparse
from array import array
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import subprocess
import tempfile

from studio_pcm_timing import inspect_trace
from studio_pcm_content import inspect_content

SHAPES = ((1280, 720), (720, 1280), (1920, 1080), (1080, 1920))


def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def run_session(exe, directory, width, height, seconds, trace, precise):
    directory.mkdir()
    with (directory / 'studio.log').open('wb') as log:
        return subprocess.run([exe, directory, str(width), str(height), str(seconds), *(['--pcm-trace'] if trace else []),
                               *(['--precise-poll'] if precise else [])],
                              timeout=seconds + 30, stdout=log, stderr=subprocess.STDOUT).returncode


def inspect_tone_windows(pcm):
    # Skip AAC boundary padding, then check every complete non-overlapping 10 ms
    # window. The fixture is 48 kHz stereo, with opposite-phase 500 Hz channels.
    from operator import mul
    samples = len(pcm)//2
    if len(pcm) % 2 or samples < 2*2048 + 480:
        raise ValueError('missing or incomplete stereo audio')
    phase = [2*math.pi*i/96 for i in range(480)]
    sine, cosine = list(map(math.sin, phase)), list(map(math.cos, phase))
    windows = range(2048, samples - 2048 - 480 + 1, 480)
    for pos in windows:
        left, right = pcm[2*pos:2*(pos+480):2], pcm[2*pos+1:2*(pos+480):2]
        for channel in (left, right):
            amp = math.hypot(sum(map(mul, channel, sine)), sum(map(mul, channel, cosine))) / (240*32768)
            if not .15 < amp < .45:
                raise ValueError(f'missing stereo tone in decoded window at sample {pos}')
        if sum(map(mul, left, right)) >= 0:
            raise ValueError(f'wrong stereo phase in decoded window at sample {pos}')
    return len(windows)


def inspect(ffmpeg, ffprobe, directory, width, height, seconds, m, *, first_frame=0, record_slots=None):
    path = directory / 'take.mp4'
    probe = json.loads(subprocess.check_output([ffprobe, '-v', 'error', '-show_streams',
                                                '-of', 'json', path], timeout=15))
    video, = [s for s in probe['streams'] if s['codec_type'] == 'video']
    audio, = [s for s in probe['streams'] if s['codec_type'] == 'audio']
    if ((video['width'], video['height'], video['codec_name']) != (width, height, 'h264') or
            (audio['codec_name'], int(audio['sample_rate']), audio['channels']) != ('aac', 48000, 2)):
        raise ValueError('unexpected encoded track layout')
    cmd = [ffmpeg, '-nostdin', '-v', 'error', '-xerror', '-i', path]
    pixels = subprocess.check_output(cmd + ['-map', '0:v:0', '-vf',
        'crop=iw:ih/4:0:0,scale=8:1:flags=neighbor', '-fps_mode', 'passthrough',
        '-pix_fmt', 'gray', '-f', 'rawvideo', 'pipe:1'], timeout=2*seconds+30)
    if not pixels or len(pixels) % 8:
        raise ValueError('missing or partial frame signature')
    n = len(pixels) // 8
    for i in range(n):
        bits = pixels[i*8:(i+1)*8]
        if any(80 < v < 176 for v in bits) or sum((v >= 176) << j for j, v in enumerate(bits)) != (first_frame + i) % 256:
            raise ValueError(f'wrong program frame at {i}: {list(bits)}')
    pcm = array('h')
    pcm.frombytes(subprocess.check_output(cmd + ['-map', '0:a:0', '-f', 's16le', 'pipe:1'],
                                          timeout=seconds+30))
    samples = len(pcm)//2
    m.update(decodedFrames=n, decodedStereoSamples=samples, decodedToneWindows=inspect_tone_windows(pcm),
             measuredPreviewFps=m['previewFrames']*1e6/m['runUs'])
    nodes = m['received']
    if m['previewFrames'] != nodes['studio.video-output'] or nodes['studio.audio-output'] != nodes['studio.monitor']:
        raise ValueError('live branch counters disagree')
    v, a = m['programVideo'], m['programAudio']
    if (not v['consumed'] or v['accepted'] + v['rejected'] != nodes['studio.video-output'] or
            v['consumed'] + v['dropped'] != v['accepted'] or v['badMarkers'] or
            v['markerGaps'] > v['dropped'] + v['rejected']):
        raise ValueError('program I420 accounting or frame markers disagree')
    if (not a['verifiedBlocks'] or a['badBlocks'] or a['verifiedBlocks']*480 != a['consumedSamples'] or
            a['consumedSamples'] + a['droppedSamples'] != a['acceptedSamples'] or
            a['acceptedSamples'] + 480*a['rejectedWrites'] != 480*nodes['studio.audio-output']):
        raise ValueError('program PCM accounting or content disagrees')
    m['clockSkipped'] = m['dropped'] - v['rejected']
    if m['clockSkipped'] < 0:
        raise ValueError('program rejection missing from dropped count')
    if m['overloaded'] or m['recordError'] or m['graphError'] or m['recordBusyAfterRelease']:
        raise ValueError('session failed or did not finalize')
    if record_slots is None:
        record_slots = nodes['studio.audio-output']
    if n != m['previewFrames'] - first_frame or samples < 480*record_slots-480:
        raise ValueError('recording is shorter than the admitted program')
    if m['runUs'] < seconds*1_000_000:
        raise ValueError('session stopped before requested duration')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('exe', type=Path)
    parser.add_argument('ffmpeg', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--seconds', type=int, default=3)
    parser.add_argument('--sessions', type=int, default=1, help='concurrent Studio processes of the same shape')
    parser.add_argument('--pcm-trace', action='store_true', help='record and reconcile per-slot monotonic timings')
    parser.add_argument('--precise-poll', action='store_true', help='experiment only: private 2 ms Windows timer')
    args = parser.parse_args()
    if not 2 <= args.seconds <= 180 or not 1 <= args.sessions <= 4:
        parser.error('expected 2..180 seconds and 1..4 sessions')
    exe, ffmpeg = args.exe.resolve(strict=True), args.ffmpeg.resolve(strict=True)
    ffprobe = ffmpeg.with_name('ffprobe.exe').resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix='studio-', dir=args.output.resolve()))
    print(f'Studio capacity evidence: {run}', flush=True)
    info = {'os': platform.platform(), 'cpu': platform.processor(), 'logicalCpus': os.cpu_count(),
            'renderer': 'D3D11 WARP', 'source': '320x180 remote synthetic video and stereo PCM',
            'experimentalPoll': args.precise_poll, 'pcmTrace': args.pcm_trace, 'secondsPerSession': args.seconds, 'processesPerShape': args.sessions,
            'runtimeSha256': {p.name: digest(p) for p in (exe, ffmpeg, ffprobe)},
            'passed': False, 'policy': 'functional-content-only; no deployment timing SLA',
            'shapes': []}
    try:
        for width, height in SHAPES:
            row = {'width': width, 'height': height, 'sessions': [], 'errors': []}
            info['shapes'].append(row)
            dirs = [run/f'{width}x{height}-{i+1}' for i in range(args.sessions)]
            with ThreadPoolExecutor(max_workers=args.sessions) as pool:
                tasks = [pool.submit(run_session, exe, d, width, height, args.seconds, args.pcm_trace, args.precise_poll) for d in dirs]
            for d, task in zip(dirs, tasks):
                m = {'directory': d.name}
                row['sessions'].append(m)
                try:
                    m['returnCode'] = task.result()
                    m.update(json.loads((d/'metrics.json').read_bytes()))
                    if m['pollMode'] != ('experimental-precise' if args.precise_poll else 'production') or m['processCpuUs'] < 0:
                        raise ValueError('wrong poll mode or invalid process CPU counter')
                    if args.pcm_trace:
                        m['pcmContent'] = inspect_content(d, m)
                        m['pcmTiming'] = inspect_trace(d, m)
                    inspect(ffmpeg, ffprobe, d, width, height, args.seconds, m)
                    if args.pcm_trace and m['pcmContent']['emptyInputSlotsNs']:
                        raise ValueError('fixture PCM supply ran dry; see pcmContent')
                    if m['returnCode']:
                        raise ValueError(f'process exited with {m["returnCode"]}')
                except (OSError, ValueError, KeyError, subprocess.SubprocessError) as e:
                    row['errors'].append(f'{d.name}: {e}')
            if all('beginMs' in m for m in row['sessions']):
                overlap = min(m['endMs'] for m in row['sessions']) - max(m['beginMs'] for m in row['sessions'])
                row['commonIntervalMs'] = overlap
                if args.sessions > 1 and overlap < 0.75 * args.seconds * 1000:
                    row['errors'].append('insufficient overlap to measure concurrent sessions')
            print(width, height, [(m.get('decodedFrames'), m.get('dropped')) for m in row['sessions']], row['errors'], flush=True)
        info['passed'] = not any(row['errors'] for row in info['shapes'])
    finally:
        (run/'verification.json').write_text(json.dumps(info, indent=2), encoding='utf-8')
    if not info['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
