"""Real C++/Go PCM exchange via an isolated official server. No desktop/device use."""
import argparse
import asyncio
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

from reconnect_integration import free_port, readiness_token
from websockets.asyncio.client import connect


async def run(args):
    root = Path('out/extensions-'+args.extensions if args.extensions else ('out/go-rpc-interop' if args.rpc_only else 'out/go-interop-integration'), time.strftime('%Y%m%d-%H%M%S')).resolve()
    root.mkdir(parents=True)
    ports = set()
    while len(ports) < 3:
        ports.add(free_port())
    port, tcp, udp = ports
    url = f'ws://127.0.0.1:{port}'
    config = root / 'server.yaml'
    config.write_text(f'port: {port}\nbind_addresses: [127.0.0.1]\nrtc:\n  tcp_port: {tcp}\n  udp_port: {udp}\n  use_external_ip: false\n  node_ip: 127.0.0.1\n  enable_loopback_candidate: true\nkeys:\n  reconnect-test: reconnect-integration-secret-32-characters\nlogging:\n  level: info\n')
    if args.extensions in ('encoded','media-e2ee'):
        subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','testsrc2=size=160x120:rate=30','-t','3','-c:v','libx264','-preset','ultrafast','-tune','zerolatency','-bf','0','-g','30',str(root/'source.h264')], check=True, timeout=15)
    processes, logs = [], []
    def start(name, argv):
        log = open(root / f'{name}.log', 'w', encoding='utf-8')
        logs.append(log)
        p = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT,
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
        go = start('go', [str(Path(args.go_peer).resolve()), '-url', url, '-out', str(root)] + (['-extensions', args.extensions] if args.extensions else (['-rpc-only'] if args.rpc_only else [])))
        deadline = time.monotonic() + 25
        while not (root / 'go-ready').exists():
            if go.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('Go peer readiness failed')
            await asyncio.sleep(.1)
        cpp = start('cpp', ([str(Path(args.client).resolve()), url, str(root), args.extensions] if args.extensions else [str(Path(args.client).resolve()), url, url, str(root), '--go-rpc-only' if args.rpc_only else '--go-interop']))
        deadline = time.monotonic() + 55
        while cpp.poll() is None or go.poll() is None:
            if (cpp.poll() not in (None, 0) or go.poll() not in (None, 0)) or time.monotonic() > deadline:
                raise RuntimeError('Interop peers failed or timed out')
            await asyncio.sleep(.2)
        if cpp.returncode != 0 or go.returncode != 0:
            raise RuntimeError(f'Interop exit codes: C++={cpp.returncode}, Go={go.returncode}')
        if args.extensions in ('backup','encoded','media-e2ee'):
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
    parser.add_argument('--extensions', choices=['plain','encrypted','single','backup','encoded','media-e2ee'])
    parser.add_argument('--rpc-only', action='store_true', help='Negotiate data channels without publishing media')
    asyncio.run(run(parser.parse_args()))
