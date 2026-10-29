"""Verify full backend rejoin; does not claim signaling resume or room migration."""
import argparse
import asyncio
import contextlib
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
from urllib.parse import parse_qs, urlsplit
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed
from sfu_test_support import free_port, wire_bytes


async def run(args):
    root = Path('out/rtc-rejoin-integration').resolve() / time.strftime('%Y%m%d-%H%M%S')
    root.mkdir(parents=True)
    port, proxy_port, tcp = free_port(), free_port(), free_port()
    config = root / 'server.yaml'
    config.write_text(json.dumps({'port': port, 'bind_addresses': ['127.0.0.1'],
        'keys': {'reconnect-test': 'reconnect-integration-secret-32-characters'},
        'rtc': {'tcp_port': tcp, 'node_ip': '127.0.0.1', 'use_external_ip': False,
                'enable_loopback_candidate': True, 'ips': {'includes': ['127.0.0.1/32']}}}))
    connections, requests = set(), []
    async def proxy(client):
        requests.append(parse_qs(urlsplit(client.request.path).query))
        try:
            async with connect(f'ws://127.0.0.1:{port}' + client.request.path, max_size=8*1024*1024,
                    additional_headers={'Authorization': client.request.headers['Authorization']},
                    proxy=None, compression=None) as upstream:
                connections.add(client)
                async def pump(source, target):
                    async for data in source:
                        await target.send(data)
                tasks = [asyncio.create_task(pump(client, upstream)), asyncio.create_task(pump(upstream, client))]
                try:
                    await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
                finally:
                    for task in tasks:
                        task.cancel()
                    await asyncio.gather(*tasks, return_exceptions=True)
        except ConnectionClosed:
            pass
        finally:
            connections.discard(client)

    async def interrupt():
        for marker in ('rejoin-1', 'rejoin-2', 'terminal'):
            while not (root / marker).exists():
                await asyncio.sleep(.02)
            for client in tuple(connections):
                if marker == 'rejoin-2':
                    await client.close(code=1012, reason='test signal interruption')
                else:
                    # PARTICIPANT_REMOVED remains terminal even with RECONNECT action.
                    await client.send(wire_bytes(8, bytes.fromhex('10041802' if marker == 'terminal' else '10061802')))

    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    with (root / 'server.log').open('w') as log:
        server = subprocess.Popen([str(Path(args.server).resolve()), '--config', str(config)],
                                  stdout=log, stderr=log, creationflags=flags)
        try:
            deadline = time.monotonic() + 20
            while True:
                assert server.poll() is None, f'Server stopped: {root}'
                try:
                    reader, writer = await asyncio.open_connection('127.0.0.1', port)
                    writer.close()
                    await writer.wait_closed()
                    break
                except OSError:
                    assert time.monotonic() < deadline, 'Server readiness timeout'
                    await asyncio.sleep(.1)
            async with serve(proxy, '127.0.0.1', proxy_port, max_size=8*1024*1024):
                cutter = asyncio.create_task(interrupt())
                try:
                    with (root / 'client.stdout.log').open('w') as out, (root / 'client.stderr.log').open('w') as err:
                        client = await asyncio.create_subprocess_exec(str(Path(args.client).resolve()),
                            f'http://127.0.0.1:{port}', f'ws://127.0.0.1:{proxy_port}', str(root),
                            stdout=out, stderr=err, creationflags=flags)
                        try:
                            code = await asyncio.wait_for(client.wait(), 90)
                        finally:
                            if client.returncode is None:
                                client.kill()
                                await client.wait()
                    assert code == 0, f'Client failed: {root}'
                    await cutter
                finally:
                    cutter.cancel()
                    with contextlib.suppress(asyncio.CancelledError):
                        await cutter
            assert len(requests) == 3 and all(q.get('reconnect') != ['1'] for q in requests), requests
            report = json.loads((root / 'client.stdout.log').read_text())
            report.update(fullJoins=len(requests), resumeRequests=0,
                clientSHA256=hashlib.sha256(Path(args.client).read_bytes()).hexdigest(),
                serverSHA256=hashlib.sha256(Path(args.server).read_bytes()).hexdigest())
            (root / 'result.json').write_text(json.dumps(report, indent=2))
            print(json.dumps(report), root)
        finally:
            server.terminate()
            try:
                await asyncio.to_thread(server.wait, 5)
            except subprocess.TimeoutExpired:
                server.kill()
                await asyncio.to_thread(server.wait)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--client', required=True)
    parser.add_argument('--server', required=True)
    asyncio.run(run(parser.parse_args()))
