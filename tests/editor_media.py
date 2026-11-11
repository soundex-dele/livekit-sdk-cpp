"""Synthetic, device-free FreeCut fixtures and encoded-output verification."""
import array
import json
import math
from pathlib import Path
import subprocess
import sys

mode, executable, directory = sys.argv[1:]
ffmpeg = Path(executable)
root = Path(directory)
root.mkdir(parents=True, exist_ok=True)


def run(*args):
    return subprocess.run([str(ffmpeg), '-v', 'error', '-y', *map(str, args)],
                          check=True, stdout=subprocess.PIPE).stdout


if mode == 'fixtures':
    run('-f', 'lavfi', '-i', 'color=c=red:s=160x90:r=30:d=2',
        '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=2',
        '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-shortest', root / 'red.mp4')
    run('-f', 'lavfi', '-i', 'color=c=blue:s=160x90:d=1', '-frames:v', '1', root / 'blue.png')
    run('-f', 'lavfi', '-i', 'sine=frequency=880:sample_rate=44100:duration=2', root / 'tone.wav')
    run('-f', 'lavfi', '-i', 'color=c=green:s=160x90:r=30:d=1',
        '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=2',
        '-vf', 'setpts=PTS+0.5/TB', '-fps_mode', 'passthrough',
        '-c:v', 'libx264', '-c:a', 'aac', root / 'offset.mp4')
else:
    probe = ffmpeg.with_name('ffprobe' + ffmpeg.suffix)
    info = json.loads(subprocess.check_output([str(probe), '-v', 'error', '-show_streams',
                                               '-show_format', '-of', 'json', str(root / 'output.mp4')]))
    video = next(s for s in info['streams'] if s['codec_type'] == 'video')
    audio = next(s for s in info['streams'] if s['codec_type'] == 'audio')
    assert video['codec_name'] == 'h264' and audio['codec_name'] == 'aac'
    assert (video['width'], video['height'], video['r_frame_rate']) == (160, 90, '30/1')
    assert audio['sample_rate'] == '48000' and audio['channels'] == 2
    assert abs(float(info['format']['duration']) - 2) <= 1 / 30
    for time, overlay in [(0.1, False), (0.75, True), (1.75, False)]:
        raw = run('-ss', time, '-i', root / 'output.mp4', '-frames:v', '1', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-')
        pixel = raw[(45 * 160 + 80) * 3:][:3]
        assert pixel[0] > 90 and (pixel[2] > 90 if overlay else pixel[2] < 40), (time, pixel)
    pcm = array.array('f', run('-i', root / 'output.mp4', '-vn', '-ar', '48000', '-ac', '1', '-f', 'f32le', '-'))
    assert abs(len(pcm) / 48000 - 2) <= 1 / 30
    samples = pcm[4800:14400]
    for hz in [440, 880]:
        energy = abs(sum(v * complex(math.cos(2 * math.pi * hz * i / 48000),
                                     math.sin(2 * math.pi * hz * i / 48000))
                         for i, v in enumerate(samples))) / len(samples)
        assert energy > .005, (hz, energy)
    assert not list(root.glob('*.tmp.mp4')), 'Temporary exports leaked'
    fractional = json.loads(subprocess.check_output([str(probe), '-v', 'error', '-show_streams',
                                                    '-show_format', '-of', 'json', str(root / 'fractional.mp4')]))
    fractional_video = next(s for s in fractional['streams'] if s['codec_type'] == 'video')
    assert fractional_video['r_frame_rate'] == '30000/1001'
    assert abs(float(fractional['format']['duration']) - 1.001) < 1001 / 30000
    print('H.264/AAC, duration, three composition positions and both audio tones verified.')
