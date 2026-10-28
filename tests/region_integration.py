"""Trusted LiveKit redirects and bounded HTTP discovery; isolated test processes."""
import argparse
import asyncio
import hashlib
import base64
import hmac
from http import HTTPStatus
import json
import os
from pathlib import Path
import subprocess
import time
from websockets.asyncio.server import serve
from websockets.asyncio.client import connect
from reconnect_integration import free_port
from protocol_contract import fields, first


def varint(value):
    result = bytearray()
    while value >= 128:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def field(number, value):
    return varint(number * 8 + 2) + varint(len(value)) + value


def readiness_token():
    def encode(value):
        return base64.urlsafe_b64encode(value).rstrip(b'=')
    header = encode(b'{"alg":"HS256","typ":"JWT"}')
    payload = encode(json.dumps({'iss': 'region-test', 'sub': 'readiness', 'exp': int(time.time()) + 60,
        'video': {'roomJoin': True, 'room': 'region-readiness'}}).encode())
    unsigned = header + b'.' + payload
    signature = encode(hmac.new(b'region-integration-secret-32-characters', unsigned, hashlib.sha256).digest())
    return (unsigned + b'.' + signature).decode()


async def run(args):
    root = Path('out/region-integration', time.strftime('%Y%m%d-%H%M%S')).resolve()
    root.mkdir(parents=True)
    ports = set()
    while len(ports) < 5:
        ports.add(free_port())
    port, rtc, udp, proxy_port, http_port = ports
    server_url, fixture_url = f'http://127.0.0.1:{port}', f'http://127.0.0.1:{proxy_port}'
    http_url = f'http://127.0.0.1:{http_port}'
    forbidden_requests = 0
    config = root / 'server.yaml'
    config.write_text(f'port: {port}\nbind_addresses: [127.0.0.1]\nrtc:\n  tcp_port: {rtc}\n  udp_port: {udp}\n  node_ip: 127.0.0.1\n  use_external_ip: false\n  enable_loopback_candidate: true\nkeys:\n  region-test: region-integration-secret-32-characters\nlogging:\n  level: info\n')
    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0

    async def process_request(connection, request):
        nonlocal forbidden_requests
        path = request.path.split('?')[0]
        if path == '/unavailable/rtc':
            return connection.respond(HTTPStatus.SERVICE_UNAVAILABLE, 'retry another test endpoint')
        if path == '/denied/rtc':
            return connection.respond(HTTPStatus.FORBIDDEN, 'permission denied')
        if path == '/unauthorized/rtc':
            return connection.respond(HTTPStatus.UNAUTHORIZED, 'authentication required')
        if path == '/forbidden/rtc':
            forbidden_requests += 1
            return connection.respond(HTTPStatus.FORBIDDEN, 'must not receive credentials')
        if path == '/http/rtc':
            response = connection.respond(HTTPStatus.FOUND, 'redirect')
            response.headers['Location'] = fixture_url + '/forbidden/rtc'
            return response
        return None

    async def http_handler(reader, writer):
        nonlocal forbidden_requests
        try:
            headers = await reader.readuntil(b'\r\n\r\n')
            path = headers.split(b'\r\n', 1)[0].split()[1].decode()
            if path.startswith('/settings-slow/'):
                try:
                    await asyncio.wait_for(reader.read(1), 2)
                    return  # cancelled HTTP request; don't send a late response
                except asyncio.TimeoutError:
                    pass
            status, extra = '200 OK', ''
            body = json.dumps({'regions': [{'region': 'test', 'url': server_url, 'distance': 0}]}).encode()
            if path.startswith('/settings-large/'):
                body = b'x' * 70000
            elif path.startswith('/settings-bad/'):
                body = b'{malformed'
            elif path.startswith('/settings-redirect/'):
                status, extra = '302 Found', f'Location: {http_url}/forbidden/settings/regions\r\n'
            elif path.startswith('/forbidden/'):
                forbidden_requests += 1
                status = '403 Forbidden'
            writer.write(f'HTTP/1.1 {status}\r\n{extra}Content-Length: {len(body)}\r\nConnection: close\r\n\r\n'.encode() + body)
            await writer.drain()
        except (ConnectionResetError, BrokenPipeError, asyncio.IncompleteReadError):
            pass  # expected client-side cancellation
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except (ConnectionResetError, BrokenPipeError):
                pass

    async def handler(connection):
        path = connection.request.path.split('?')[0]
        if path == '/media-stall/rtc':
            # Accept a real JoinResponse, then withhold SDP and ICE. The first
            # endpoint succeeds at signaling but can never establish media.
            query = connection.request.path.partition('?')[2]
            async with connect(server_url.replace('http://', 'ws://') + '/rtc?' + query,
                additional_headers={'Authorization': connection.request.headers['Authorization']},
                proxy=None, compression=None) as upstream:
                await connection.send(await asyncio.wait_for(upstream.recv(), 3))
                await connection.wait_closed()
            return
        if path in ('/silent/rtc', '/silent-2/rtc'):
            await connection.wait_closed()
            return
        if path == '/malformed/rtc':
            await connection.send(b'\xff')
            await connection.wait_closed()
            return
        if path == '/force-relay/rtc':
            join = field(2, field(1, b'PA_forced') + field(2, b'forced'))
            join += field(8, bytes.fromhex('2802'))
            await connection.send(field(1, join))
            await connection.wait_closed()
            return
        target = fixture_url + '/cycle' if path == '/cycle/rtc' else server_url
        await connection.send(field(1, field(7, target.encode())))
        await connection.wait_closed()

    with (root / 'server.log').open('w') as log:
        server = subprocess.Popen([args.server, '--config', str(config)], stdout=log, stderr=log, creationflags=flags)
        try:
            until = time.monotonic() + 20
            while True:
                if server.poll() is not None or time.monotonic() > until:
                    raise RuntimeError(f'Server startup failed: {root}')
                try:
                    _, writer = await asyncio.open_connection('127.0.0.1', port)
                    writer.close()
                    await writer.wait_closed()
                    break
                except OSError:
                    await asyncio.sleep(.1)
            # Listening is earlier than routing readiness on some server starts.
            # Wait for a real JoinResponse, without retrying the client under test.
            while True:
                try:
                    async with connect(server_url.replace('http://', 'ws://') + '/rtc?protocol=12&sdk=cpp&auto_subscribe=0',
                        additional_headers={'Authorization': 'Bearer ' + readiness_token()}, proxy=None, compression=None) as probe:
                        response = fields(await asyncio.wait_for(probe.recv(), 2))
                        if 1 not in response or 2 not in fields(first(response, 1)):
                            raise RuntimeError('Readiness probe did not receive a participant JoinResponse')
                    break
                except Exception:
                    if time.monotonic() > until:
                        raise RuntimeError(f'Server routing readiness failed: {root}')
                    await asyncio.sleep(.1)
            async with await asyncio.start_server(http_handler, '127.0.0.1', http_port), \
                serve(handler, '127.0.0.1', proxy_port, process_request=process_request, compression=None):
                with (root / 'client.stdout.log').open('w') as out, (root / 'client.stderr.log').open('w') as err:
                    client = await asyncio.create_subprocess_exec(str(Path(args.client).resolve()), server_url, fixture_url, http_url,
                        stdout=out, stderr=err, creationflags=flags)
                    try:
                        code = await asyncio.wait_for(client.wait(), 90)
                    finally:
                        if client.returncode is None:
                            client.kill()
                            await client.wait()
                if code:
                    raise RuntimeError(f'Client failed: {root}')
            if forbidden_requests:
                raise RuntimeError('A rejected HTTP redirect received a request')
            result = json.loads((root / 'client.stdout.log').read_text().strip().splitlines()[-1])
            result['forbiddenRequests'] = forbidden_requests
            result['clientSHA256'] = hashlib.sha256(Path(args.client).read_bytes()).hexdigest()
            result['serverSHA256'] = hashlib.sha256(Path(args.server).read_bytes()).hexdigest()
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
    asyncio.run(run(parser.parse_args()))
