"""Run the public C++ SDK directly in one process against an isolated local server."""
import argparse
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import time
import wave
from integration import free_port

p = argparse.ArgumentParser()
p.add_argument('--client', required=True)
p.add_argument('--server', required=True)
args = p.parse_args()
root = Path('out/sdk-integration').resolve() / time.strftime('%Y%m%d-%H%M%S')
root.mkdir(parents=True)
port, rtc, udp = [free_port() for _ in range(3)]
env = dict(os.environ, LIVEKIT_URL=f'http://127.0.0.1:{port}', LIVEKIT_API_KEY='sdktest',
           LIVEKIT_API_SECRET='sdk-integration-test-secret-32-characters', LIVEKIT_TOKEN='')
config = root / 'server.yaml'
config.write_text(f'port: {port}\nbind_addresses: [127.0.0.1]\nrtc:\n  tcp_port: {rtc}\n  udp_port: {udp}\n  use_external_ip: false\n  node_ip: 127.0.0.1\n  enable_loopback_candidate: true\nkeys:\n  sdktest: {env["LIVEKIT_API_SECRET"]}\nlogging:\n  level: warn\n')
flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
with open(root / 'server.log', 'w') as log:
    server = subprocess.Popen([args.server, '--config', str(config)], stdout=log, stderr=log, creationflags=flags)
    try:
        deadline = time.monotonic() + 20
        while True:
            if server.poll() is not None:
                raise RuntimeError('Server exited; see ' + str(root))
            try:
                with socket.create_connection(('127.0.0.1', port), timeout=.5):
                    break
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(.1)
        with open(root / 'client.stdout.log', 'w', encoding='utf-8') as out, open(root / 'client.stderr.log', 'w', encoding='utf-8') as err:
            result = subprocess.run([str(Path(args.client).resolve()), str(root / 'recordings')],
                                    env=env, stdout=out, stderr=err, timeout=100, creationflags=flags)
        assert result.returncode == 0, f'Client failed; see {root}'
        recording = root / 'recordings'
        assert max(f.stat().st_size for f in recording.glob('*.y4m')) > 640 * 360 * 3 // 2 * 20
        audible = False
        for f in recording.glob('*.wav'):
            with wave.open(str(f)) as w:
                samples = w.readframes(w.getnframes())
                pcm = struct.unpack(f'<{len(samples)//2}h', samples)
                audible |= len(pcm) > 48000 and max(abs(s) for s in pcm) > 100
        assert audible, 'Expected decoded non-silent audio'
        report = json.loads((root / 'client.stdout.log').read_text())
        report['recordingsVerified'] = True
        (root / 'result.json').write_text(json.dumps(report, indent=2))
        print(json.dumps(report), root)
    finally:
        server.terminate()
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
