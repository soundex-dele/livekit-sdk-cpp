"""Reopen real finalized MP4 outside the producer; check decoded video identities and PCM."""
from array import array
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile


def decode(ffmpeg, path, *options):
    return subprocess.check_output([ffmpeg, '-nostdin', '-v', 'error', '-xerror', '-i', path,
                                    *options, 'pipe:1'], timeout=15)


def check_file(ffmpeg, path, prefix_only=False):
    video = decode(ffmpeg, path, '-map', '0:v:0', '-fps_mode', 'passthrough', *(['-frames:v', '32'] if prefix_only else []), '-pix_fmt', 'gray', '-f', 'rawvideo')
    stride = 64 * 48
    if len(video) != 32 * stride:
        raise ValueError(f'{path}: unexpected decoded video length: {len(video)}')
    for n in range(32):
        barcode = sum((video[n*stride+24*64+i*8+4] > 128) << i for i in range(8))
        if barcode != n:
            raise ValueError(f'{path}: frame {n} decoded as {barcode}')
    pcm = array('f')
    pcm.frombytes(decode(ffmpeg, path, '-map', '0:a:0', '-ac', '2', '-ar', '48000', *(['-t', '1.5'] if prefix_only else []), '-f', 'f32le'))
    left, right = pcm[0::2], pcm[1::2]
    if not 61440 <= len(left) <= 61440 + 24000 or len(left) != len(right):
        raise ValueError(f'{path}: unexpected decoded stereo sample count {len(left)}')
    onset = next(i for i, v in enumerate(left) if abs(v) > .12)
    for n in range(32):
        start = onset + 1920*n + 448
        window = left[start:start+1024]
        hz = 400 if n < 16 else 1200 if n < 31 else 1900
        wave = [2*math.pi*hz*i/48000 for i in range(1024)]
        amplitude = 2*math.hypot(sum(v*math.sin(t) for v,t in zip(window,wave)),
                                 sum(v*math.cos(t) for v,t in zip(window,wave)))/1024
        opposite = sum(a*b for a,b in zip(window,right[start:start+1024]))
        if len(window)!=1024 or not .15 < amplitude < .5 or not opposite < -8:
            raise ValueError(f'{path}: bad stereo block {n}, tone amplitude {amplitude}, L*R {opposite}')
    return {'videoFrames':32,'decodedStereoSamples':len(left),'pcmBlocksVerified':32}


def main():
    exe, ffmpeg, output = sys.argv[1:]
    base = Path(output); base.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(dir=base, prefix='take-'))
    print(f'Recording evidence: {run}', flush=True)
    (run/'failed.mp4.partial').mkdir()  # Force a real filesystem error during MP4 remux.
    subprocess.run([exe, run], check=True, timeout=20)
    result = {file: check_file(ffmpeg,run/file) for file in ('record.mp4','failed.mkv')}
    written, reason = map(int, (run/'locked-write.txt').read_text().split())
    if written < 160 or reason >= 0 or (run/'locked.mp4').exists():
        raise ValueError('missing real mid-record write failure or incomplete MP4 published')
    result['locked.mkv'] = {'encoderCallsCompleted': written, 'ffmpegError': reason,
                            'verifiedPrefix': check_file(ffmpeg, run/'locked.mkv', prefix_only=True)}
    direct_video = decode(ffmpeg,run/'standalone.mp4','-map','0:v:0','-fps_mode','passthrough','-pix_fmt','gray','-f','rawvideo')
    direct_pcm = decode(ffmpeg,run/'standalone.mp4','-map','0:a:0','-ac','2','-ar','48000','-f','f32le')
    if len(direct_video) != 64*48 or len(direct_pcm) < 1920*8:
        raise ValueError('standalone recorder failed to finalize its video/audio')
    result['standalone.mp4'] = {'videoFrames':1, 'decodedStereoSamples':len(direct_pcm)//8}
    if (run/'failed.mp4').exists():
        raise ValueError('finalization failure published an MP4')
    (run/'verification.json').write_text(json.dumps(result, indent=2),encoding='utf-8')
    print(result)


if __name__ == '__main__':
    main()
