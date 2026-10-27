"""RoomMoved wire fixture plus real destination rejoin/media on an isolated server.

--mode server attempts the official MoveParticipant API instead of injection.
An unsupported server is a failure, never silently counted as a migration pass.
"""
import argparse
import asyncio
import base64
import hashlib
import hmac
import json
import os
from pathlib import Path
import subprocess
import time
from urllib.request import Request, urlopen
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
from reconnect_integration import free_port, wire_bytes
from region_integration import varint
from protocol_contract import fields, first


def token(identity, room, admin=False):
    def b64(value):
        return base64.urlsafe_b64encode(value).rstrip(b'=')
    grants = {'room': room, 'roomJoin': not admin, 'roomAdmin': admin, 'canPublish': True,
              'canSubscribe': True, 'canPublishData': True}
    if admin:
        grants['destinationRoom'] = 'move-target'
    payload = {'iss': 'move-test', 'sub': identity, 'exp': int(time.time()) + 120, 'video': grants}
    unsigned = b64(b'{"alg":"HS256","typ":"JWT"}') + b'.' + b64(json.dumps(payload).encode())
    return (unsigned + b'.' + b64(hmac.new(b'move-integration-secret-32-characters', unsigned, hashlib.sha256).digest())).decode()


def number(field, value):
    return varint(field * 8) + varint(int(value))


def string(field, value):
    return wire_bytes(field, value.encode())


def participant(info):
    result = string(1, info['sid']) + string(2, info['identity']) + number(3, 2)
    result += string(5, info.get('metadata', '')) + string(9, info.get('name', ''))
    for track in info.get('tracks', []):
        data = string(1, track['sid']) + number(2, track['type']) + string(3, track['name'])
        data += number(4, track['muted']) + number(5, track['width']) + number(6, track['height'])
        data += number(9, track['source']) + string(11, track['mimeType'])
        result += wire_bytes(4, data)
    return result


async def run(args):
    root = Path('out/room-moved-integration', time.strftime('%Y%m%d-%H%M%S')).resolve()
    root.mkdir(parents=True)
    ports = set()
    while len(ports) < 4:
        ports.add(free_port())
    port, rtc, udp, proxy_port = ports
    config = root / 'server.yaml'
    config.write_text(f'port: {port}\nbind_addresses: [127.0.0.1]\nrtc:\n  tcp_port: {rtc}\n  udp_port: {udp}\n  node_ip: 127.0.0.1\n  use_external_ip: false\n  enable_loopback_candidate: true\nkeys:\n  move-test: move-integration-secret-32-characters\nlogging:\n  level: info\n')
    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    connections = set()
    requested_rooms = []

    async def proxy(client):
        auth = client.request.headers['Authorization']
        encoded = auth.split()[1].split('.')[1]
        requested_rooms.append(json.loads(base64.urlsafe_b64decode(encoded + '=' * (-len(encoded) % 4)))['video']['room'])
        async with connect(f'ws://127.0.0.1:{port}{client.request.path}',
            additional_headers={'Authorization': auth}, proxy=None, compression=None) as upstream:
            connections.add(client)
            async def forward(source, destination):
                async for message in source:
                    await destination.send(message)
            tasks = [asyncio.create_task(forward(client, upstream)), asyncio.create_task(forward(upstream, client))]
            try:
                await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            finally:
                connections.discard(client)
                for task in tasks:
                    task.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)

    def server_move():
        request = Request(f'http://127.0.0.1:{port}/twirp/livekit.RoomService/MoveParticipant',
            data=json.dumps({'room': 'move-source', 'identity': 'mover', 'destination_room': 'move-target'}).encode(),
            headers={'Authorization': 'Bearer ' + token('test-controller', 'move-source', True), 'Content-Type': 'application/json'})
        with urlopen(request, timeout=5) as response:
            response.read()

    async def scenario():
        while not (root / 'move-ready').exists():
            await asyncio.sleep(.02)
        if args.mode == 'server':
            await asyncio.to_thread(server_move)
        else:
            snapshot = json.loads((root / 'move.json').read_text())
            if args.mode == 'mismatch':
                snapshot['participant']['tracks'] = []
            room = snapshot['room']
            move = wire_bytes(1, string(1, room['sid']) + string(2, room['name']) + string(8, room['metadata']))
            move += string(2, token('mover', 'move-target')) + wire_bytes(3, participant(snapshot['participant']))
            for remote in snapshot['otherParticipants']:
                move += wire_bytes(4, participant(remote))
            for client in tuple(connections):
                await client.send(wire_bytes(24, move))
                if args.mode != 'mismatch':
                    await client.send(wire_bytes(24, move))  # duplicate must not re-invalidate handles
        while not (root / 'cut').exists():
            await asyncio.sleep(.02)
        for client in tuple(connections):
            await client.send(bytes.fromhex('420410061802'))  # full rejoin must use destination token

    with (root / 'server.log').open('w') as log:
        server = subprocess.Popen([args.server, '--config', str(config)], stdout=log, stderr=log, creationflags=flags)
        try:
            deadline = time.monotonic() + 20
            while True:
                try:
                    async with connect(f'ws://127.0.0.1:{port}/rtc?protocol=12&sdk=cpp&auto_subscribe=0',
                        additional_headers={'Authorization': 'Bearer ' + token('readiness', 'move-readiness')}, proxy=None, compression=None) as probe:
                        response = fields(await asyncio.wait_for(probe.recv(), 2))
                        if 1 not in response or 2 not in fields(first(response, 1)):
                            raise RuntimeError('Not ready')
                    break
                except Exception:
                    if server.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError(f'Server routing readiness failed: {root}')
                    await asyncio.sleep(.1)
            async with serve(proxy, '127.0.0.1', proxy_port, compression=None):
                injector = asyncio.create_task(scenario())
                with (root / 'client.stdout.log').open('w') as out, (root / 'client.stderr.log').open('w') as err:
                    client = await asyncio.create_subprocess_exec(str(Path(args.client).resolve()),
                        f'http://127.0.0.1:{port}', f'http://127.0.0.1:{proxy_port}', str(root),
                        *(['rebuild'] if args.mode == 'mismatch' else []), stdout=out, stderr=err, creationflags=flags)
                    try:
                        code = await asyncio.wait_for(client.wait(), 60)
                        if injector.done():
                            await injector
                        if code:
                            raise RuntimeError(f'Client failed: {root}')
                    finally:
                        injector.cancel()
                        await asyncio.gather(injector, return_exceptions=True)
                        if client.returncode is None:
                            client.kill()
                            await client.wait()
            expected_rooms = ['move-source', 'move-target'] + (['move-target'] if args.mode == 'mismatch' else [])
            assert requested_rooms == expected_rooms, requested_rooms
            result = json.loads((root / 'client.stdout.log').read_text())
            result.update(migrationSource='official-server' if args.mode == 'server' else 'injected-protobuf',
                actualDestinationRejoin=True, destinationTokenUsed=True,
                clientSHA256=hashlib.sha256(Path(args.client).read_bytes()).hexdigest(),
                serverSHA256=hashlib.sha256(Path(args.server).read_bytes()).hexdigest())
            (root / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
            print(json.dumps(result), root)
        finally:
            server.terminate()
            try:
                await asyncio.to_thread(server.wait, 10)
            except subprocess.TimeoutExpired:
                server.kill()
                await asyncio.to_thread(server.wait)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--client', required=True)
    parser.add_argument('--server', required=True)
    parser.add_argument('--mode', choices=['fixture', 'mismatch', 'server'], default='fixture')
    asyncio.run(run(parser.parse_args()))
