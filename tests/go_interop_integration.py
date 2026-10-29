"""C++/Go exchange via an isolated, explicitly supplied SFU. No device use."""
import argparse
import base64
import hmac
import asyncio
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import tempfile

from sfu_test_support import free_port, readiness_token
from websockets.asyncio.client import connect


async def run(args):
    parent = Path('out/extensions-'+args.extensions).resolve()
    parent.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix=time.strftime('%Y%m%d-%H%M%S-'), dir=parent))
    ports = set()
    while len(ports) < 3:
        ports.add(free_port())
    port, tcp, udp = ports
    url = f'ws://127.0.0.1:{port}'
    config = root / 'server.yaml'
    config.write_text(f'port: {port}\nbind_addresses: [127.0.0.1]\nrtc:\n  tcp_port: {tcp}\n  udp_port: {udp}\n  use_external_ip: false\n  node_ip: 127.0.0.1\n  enable_loopback_candidate: true\nkeys:\n  reconnect-test: reconnect-integration-secret-32-characters\nlogging:\n  level: info\n')
    processes, logs = [], []
    def start(name, argv):
        env = dict(os.environ)
        if name == 'go' and args.runtime_bin:
            env['PATH'] = str(Path(args.runtime_bin).resolve()) + os.pathsep + env.get('PATH', '')
        if name == 'cpp':
            def encode(value):
                return base64.urlsafe_b64encode(json.dumps(value, separators=(',', ':')).encode()).rstrip(b'=')
            message = encode({'alg': 'HS256', 'typ': 'JWT'}) + b'.' + encode({
                'iss': 'reconnect-test', 'sub': 'cpp-peer', 'exp': int(time.time()) + 300,
                'video': {'roomJoin': True, 'room': 'extensions'}})
            signature = base64.urlsafe_b64encode(hmac.new(b'reconnect-integration-secret-32-characters', message, hashlib.sha256).digest()).rstrip(b'=')
            env['LIVEKIT_TOKEN'] = (message + b'.' + signature).decode()
        log = open(root / f'{name}.log', 'w', encoding='utf-8')
        logs.append(log)
        p = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT, env=env,
                             creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        processes.append(p)
        return p
    try:
        server = start('server', [str(Path(args.server).resolve()), '--config', str(config)])
        deadline = time.monotonic() + 25
        while True:
            try:
                async with connect(url + '/rtc?protocol=12&auto_subscribe=1', additional_headers={'Authorization': 'Bearer ' + readiness_token()}, open_timeout=2) as ws:
                    await asyncio.wait_for(ws.recv(), 3)
                break
            except Exception:
                if server.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('Server readiness failed')
                await asyncio.sleep(.2)
        go = start('go', [str(Path(args.go_peer).resolve()), '-url', url, '-out', str(root)] + ['-extensions', args.extensions])
        deadline = time.monotonic() + 25
        while not (root / 'go-ready').exists():
            if go.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError(f'Go peer readiness failed (exit {go.poll()}); see {root / "go.log"}')
            await asyncio.sleep(.1)
        cpp = start('cpp', [str(Path(args.client).resolve()), url, str(root), args.extensions])
        deadline = time.monotonic() + 55
        while cpp.poll() is None or go.poll() is None:
            if (cpp.poll() not in (None, 0) or go.poll() not in (None, 0)) or time.monotonic() > deadline:
                raise RuntimeError(f'Interop peers failed or timed out (C++={cpp.poll()}, Go={go.poll()}); see {root}')
            await asyncio.sleep(.2)
        if cpp.returncode != 0 or go.returncode != 0:
            raise RuntimeError(f'Interop exit codes: C++={cpp.returncode}, Go={go.returncode}')
        if args.extensions in ('encoded','media-e2ee'):
            subprocess.run(['ffmpeg','-v','error','-i',str(root/'go-video.h264'),'-frames:v','1','-f','null','-'],check=True,timeout=15)
        result = {'ok': True, 'cpp': json.loads((root / 'cpp-result.json').read_text()),
                  'go': json.loads((root / 'go-result.json').read_text()),
                  'sha256': {k: hashlib.sha256(Path(v).read_bytes()).hexdigest() for k, v in
                             [('server', args.server), ('cpp', args.client), ('go', args.go_peer)]}}
        (root / 'result.json').write_text(json.dumps(result, indent=2))
        print(json.dumps({'result': str(root / 'result.json'), **result}))
    finally:
        for p in reversed(processes):
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill(); p.wait(timeout=5)
        for log in logs:
            log.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', required=True)
    parser.add_argument('--client', required=True)
    parser.add_argument('--go-peer', required=True)
    parser.add_argument('--runtime-bin', help='Go peer DLL directory, e.g. the MinGW bin used by its CGO build')
    parser.add_argument('--extensions', choices=['plain','encrypted','encoded','media-e2ee'], default='plain')
    asyncio.run(run(parser.parse_args()))
