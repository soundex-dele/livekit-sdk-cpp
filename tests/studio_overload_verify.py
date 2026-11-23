"""Stall the production Studio receiver and independently decode both sessions."""
from array import array
import math
from pathlib import Path
import subprocess
import sys
import tempfile


def check_take(ffmpeg, path, indices, expected_hz, forbidden_hz):
    args = [ffmpeg, '-nostdin', '-v', 'error', '-xerror', '-i', path]
    pixels = subprocess.check_output(args + [
        '-map', '0:v:0', '-vf', 'scale=1:1:in_range=full:out_range=full',
        '-fps_mode', 'passthrough', '-pix_fmt', 'gray', '-f', 'rawvideo', 'pipe:1'], timeout=10)
    expected = [25 + 16*i for i in indices]
    if len(pixels) != len(expected) or any(abs(x-y) > 5 for x, y in zip(pixels, expected)):
        raise ValueError(f'{path.name}: decoded {list(pixels)}, expected {expected}')
    pcm = array('f')
    pcm.frombytes(subprocess.check_output(args + [
        '-map', '0:a:0', '-af', 'pan=mono|c0=c0', '-ar', '48000', '-f', 'f32le', 'pipe:1'], timeout=10))
    # Use peak per-window amplitude; AAC priming/padding is not captured sound.
    def peak(hz):
        n = 480
        phase = [2*math.pi*hz*i/48000 for i in range(n)]
        c, s = list(map(math.cos, phase)), list(map(math.sin, phase))
        blocks = (pcm[i:i+n] for i in range(0, len(pcm)-n+1, n))
        return max((2/n*math.hypot(sum(v*x for v,x in zip(b,c)), sum(v*x for v,x in zip(b,s)))
                    for b in blocks), default=0)
    if peak(expected_hz) < .15 or peak(forbidden_hz) > .06:
        raise ValueError(f'{path.name}: PCM missing or crossed session cutoff')
    print(f'{path.name}: decoded video={list(pixels)}, audio samples={len(pcm)}')


def main():
    exe, ffmpeg = sys.argv[1:]
    for fail_remux in (False, True):
        with tempfile.TemporaryDirectory(prefix='studio-overload-') as tmp:
            directory = Path(tmp)
            if fail_remux:
                (directory/'overload.mp4.partial').mkdir()
            subprocess.run([exe, directory], check=True, timeout=25)
            if fail_remux and (directory/'overload.mp4').exists():
                raise ValueError('Failed take appeared as a successful MP4')
            check_take(ffmpeg, directory/('overload.mkv' if fail_remux else 'overload.mp4'),
                       range(6), 500, 3000)
            check_take(ffmpeg, directory/'next.mp4', range(12, 14), 3000, 500)


if __name__ == '__main__':
    main()
