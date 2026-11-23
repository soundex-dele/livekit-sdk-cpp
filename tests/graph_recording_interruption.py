"""Interrupt only the recorder spawned by this test; recover its on-disk MKV into a new MP4."""
from array import array
import ctypes
from ctypes import wintypes
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def interrupt(child):
    # Use a distinctive exit code: an unrelated producer failure (1) is not a successful injection.
    api = ctypes.WinDLL('kernel32', use_last_error=True)
    api.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    api.OpenProcess.restype = wintypes.HANDLE
    api.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    api.CloseHandle.argtypes = [wintypes.HANDLE]
    h = api.OpenProcess(1, False, child.pid)  # PROCESS_TERMINATE, only our still-owned child
    if not h:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        if child.poll() is not None or not api.TerminateProcess(h, 73):
            raise RuntimeError('could not inject abrupt recorder exit')
    finally:
        api.CloseHandle(h)
    if child.wait(timeout=5) != 73:
        raise ValueError('producer did not exit due to the requested interruption')


def main():
    exe, ffmpeg, base = sys.argv[1:]
    base = Path(base); base.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(dir=base.resolve(), prefix='interrupted-'))
    print('Interrupted recording evidence:', run, flush=True)
    mkv, recovered = run/'profile.mkv', run/'recovered.mp4'
    with (run/'producer.log').open('wb') as log:
        child = subprocess.Popen([exe, '--profile', str(run), '1500'], stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 30
            while child.poll() is None and time.monotonic() < deadline:
                try:
                    with mkv.open('rb') as f:
                        length = f.seek(0, 2)  # query live file handle, not cached directory metadata
                except FileNotFoundError:
                    length = 0
                if length >= 2 * 1024 * 1024:
                    break
                time.sleep(.05)
            else:
                raise RuntimeError('producer did not write enough MKV data before timeout or exit')
            interrupt(child)
        finally:
            if child.poll() is None:
                child.kill(); child.wait(timeout=5)
    if (run/'profile.mp4').exists() or (run/'profile.mp4.partial').exists():
        raise ValueError('injection occurred outside ongoing MKV recording')
    sha = hashlib.sha256(mkv.read_bytes()).hexdigest()
    with (run/'remux.log').open('wb') as log:
        subprocess.run([ffmpeg, '-nostdin', '-v', 'warning', '-n', '-i', mkv, '-map', '0:v:0', '-map', '0:a:0',
                        '-c', 'copy', recovered], check=True, timeout=15, stdout=log, stderr=subprocess.STDOUT)
    cmd = [ffmpeg, '-nostdin', '-v', 'error', '-xerror', '-i', recovered]
    # Crop the encoded barcode, then actually decode every recovered frame, not just packet counts.
    marks = subprocess.check_output([*cmd, '-map', '0:v:0', '-vf', 'crop=128:16:0:0', '-fps_mode', 'passthrough',
                                     '-pix_fmt', 'gray', '-f', 'rawvideo', 'pipe:1'], timeout=20)
    stride = 128*16
    frames, partial = divmod(len(marks), stride)
    if partial or not 25 <= frames < 1500:
        raise ValueError(f'unexpected recovered video length: {len(marks)}')
    for i in range(frames):
        seq = sum((marks[i*stride+8*128+16*k+8] > 128) << k for k in range(8))
        if i % 256 != seq:
            raise ValueError(f'recovered video gap at {i}: {seq}')
    audio = array('f'); audio.frombytes(subprocess.check_output([*cmd, '-map', '0:a:0', '-f', 'f32le', 'pipe:1'], timeout=20))
    if len(audio) % 2 or abs(len(audio)//2 - frames*1920) > 4*1920 or max(audio, default=0) < .1:
        raise ValueError('recovered audio is empty or does not span the recovered video')
    if hashlib.sha256(mkv.read_bytes()).hexdigest() != sha:
        raise ValueError('recovery modified the original MKV')
    data = {'producerExitCode': child.returncode, 'mkvSha256': sha, 'bytesSeenBeforeInterrupt': length,
            'recoveredFrames': frames, 'recoveredSamplesPerChannel': len(audio)//2,
            'recoveredMp4Sha256': hashlib.sha256(recovered.read_bytes()).hexdigest()}
    (run/'verification.json').write_text(json.dumps(data, indent=2), encoding='utf-8')
    print(data)


if __name__ == '__main__':
    main()
