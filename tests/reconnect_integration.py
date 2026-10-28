"""Cut only test WebSockets; native media sockets and the desktop are untouched.

Requires websockets==15.0.1. Starts its own LiveKit server and loopback proxy.
"""
import argparse
import asyncio
import base64
import contextlib
import hashlib
import hmac
import json
import math
import os
import re
from pathlib import Path
import socket
import subprocess
import struct
import time
import wave
from urllib.parse import urlsplit, parse_qs
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed
from protocol_contract import fields, first


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def wire_bytes(number, data):
    def varint(value):
        result = bytearray()
        while value >= 128:
            result.append((value & 127) | 128)
            value >>= 7
        result.append(value)
        return bytes(result)
    return varint(number * 8 + 2) + varint(len(data)) + data


def wire_uint(number, value):
    result = bytearray()
    for item in (number * 8, value):
        while item >= 128:
            result.append((item & 127) | 128)
            item >>= 7
        result.append(item)
    return bytes(result)


def readiness_token():
    def encode(value):
        return base64.urlsafe_b64encode(value).rstrip(b'=')
    header = encode(b'{"alg":"HS256","typ":"JWT"}')
    payload = encode(json.dumps({'iss': 'reconnect-test', 'sub': 'readiness', 'exp': int(time.time()) + 60,
        'video': {'roomJoin': True, 'room': 'reconnect-readiness'}}).encode())
    unsigned = header + b'.' + payload
    signature = encode(hmac.new(b'reconnect-integration-secret-32-characters', unsigned, hashlib.sha256).digest())
    return (unsigned + b'.' + signature).decode()


async def run(args):
    directory = {'resume': 'reconnect-integration', 'rejoin': 'rejoin-integration', 'restart': 'server-restart-integration', 'region': 'region-rejoin-integration', 'resume-disabled': 'resume-disabled-integration', 'operations': 'operations-integration', 'video-stream': 'video-stream-integration', 'audio-stream': 'audio-stream-integration', 'video-source': 'video-source-integration', 'audio-source': 'audio-source-integration'}[args.scenario]
    root = Path('out', directory).resolve() / time.strftime('%Y%m%d-%H%M%S')
    root.mkdir(parents=True)
    if args.scenario == 'audio-stream':
        for frequency in (440, 880):
            with wave.open(str(root / f'{frequency}.wav'), 'wb') as audio:
                audio.setparams((1, 2, 48000, 48000, 'NONE', 'not compressed'))
                audio.writeframes(struct.pack('<48000h', *(int(6000 * math.sin(2 * math.pi * frequency * i / 48000)) for i in range(48000))))
    ports = set()
    while len(ports) < 3:
        ports.add(free_port())
    port, rtc, proxy_port = ports
    # TCP availability does not imply UDP availability on Windows (including
    # excluded UDP ranges). Probe the protocol the media listener will use.
    udp = free_port(socket.SOCK_DGRAM)
    while udp in ports:
        udp = free_port(socket.SOCK_DGRAM)
    config = root / 'server.yaml'
    config.write_text(f'port: {port}\nbind_addresses: [127.0.0.1]\nrtc:\n  tcp_port: {rtc}\n  udp_port: {udp}\n  use_external_ip: false\n  node_ip: 127.0.0.1\n  ips:\n    includes: [127.0.0.1/32]\n  enable_loopback_candidate: true\nkeys:\n  reconnect-test: reconnect-integration-secret-32-characters\nlogging:\n  level: info\n')
    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    connections = set()
    requests = []
    paths = []
    proxy_errors = []
    reject_resume = False
    offer_ufrags = set()
    sync_states = 0
    operation_metadata = []
    real_acknowledgements = []

    async def proxy(client):
        nonlocal reject_resume
        request = client.request
        paths.append(urlsplit(request.path).path)
        query = parse_qs(urlsplit(request.path).query)
        requests.append(query)  # contains no tokens; tokens travel in headers
        if reject_resume and query.get('reconnect') == ['1']:
            reject_resume = False
            # Official LeaveRequest: STATE_MISMATCH, action RECONNECT.
            await client.send(bytes.fromhex('420410061802'))
            await client.wait_closed()
            return
        try:
            upstream_path = request.path.removeprefix('/alternate')
            async with connect(f'ws://127.0.0.1:{port}{upstream_path}',
                               additional_headers={'Authorization': request.headers['Authorization']},
                               proxy=None, compression=None) as upstream:
                connections.add(client)
                metadata_requests = {}
                cancelled_cid = None
                dropped_cid = None

                async def release_when_ready(name, data):
                    (root / f'held-{name}').touch()
                    while not (root / f'release-{name}').exists():
                        await asyncio.sleep(.01)
                    await client.send(data)
                    (root / f'released-{name}').touch()

                releases = []

                async def pump(source, destination):
                    nonlocal sync_states, cancelled_cid, dropped_cid
                    async for data in source:
                        if args.scenario == 'audio-source' and isinstance(data, bytes):
                            signal = fields(data)
                            for tag in ((1, 2) if source is client else (2, 3)):
                                if tag in signal:
                                    description = fields(first(signal, tag))
                                    sdp = first(description, 2, b'').decode()
                                    details = [line for line in sdp.splitlines() if line.startswith(('m=audio', 'a=mid:', 'a=rtpmap:', 'a=fmtp:'))]
                                    with (root / 'audio-sdp.jsonl').open('a') as log:
                                        log.write(json.dumps({'fromClient': source is client, 'tag': tag, 'lines': details}) + '\n')
                        if args.scenario == 'operations' and isinstance(data, bytes):
                            signal = fields(data)
                            if source is client:
                                if 15 in signal:
                                    metadata = fields(first(signal, 15))
                                    name = first(metadata, 1, b'').decode()
                                    request_id = first(metadata, 4, 0)
                                    operation_metadata.append(name)
                                    metadata_requests[request_id] = name
                                    assert name != 'must-not-send', 'Cancelled operation reached server'
                                    if name == 'inject-denied':
                                        await client.send(wire_bytes(22, wire_uint(1, request_id) + wire_uint(2, 2)))
                                        continue
                                if 4 in signal:
                                    publication = fields(first(signal, 4))
                                    if first(publication, 2) == b'cancelled-publication':
                                        cancelled_cid = first(publication, 1)
                                    if first(publication, 2) == b'drop-publication':
                                        dropped_cid = first(publication, 1)
                            else:
                                if 22 in signal:
                                    response = fields(first(signal, 22))
                                    request_id = first(response, 1, 0)
                                    name = metadata_requests.get(request_id)
                                    real_acknowledgements.append({'metadata': name, 'reason': first(response, 2, 0)})
                                    if name in ('hold-cancel', 'hold-queued'):
                                        if name == 'hold-queued':
                                            await client.send(wire_bytes(22, wire_uint(1, request_id) + wire_uint(2, 4)))
                                        releases.append(asyncio.create_task(release_when_ready(name[5:], data)))
                                        continue
                                    if name == 'drop-ack':
                                        continue
                                if 6 in signal:
                                    publication = fields(first(signal, 6))
                                    if dropped_cid is not None and first(publication, 1) == dropped_cid:
                                        (root / 'dropped-publication').touch()
                                        continue
                                    if cancelled_cid is not None and first(publication, 1) == cancelled_cid:
                                        releases.append(asyncio.create_task(release_when_ready('publication', data)))
                                        continue
                        if source is upstream and args.scenario == 'resume-disabled' and isinstance(data, bytes):
                            response = fields(data)
                            if 1 in response:
                                data = wire_bytes(1, first(response, 1) + wire_bytes(8, bytes.fromhex('1801')))
                        if source is client and isinstance(data, bytes):
                            signal = fields(data)
                            if 1 in signal:
                                sdp = first(fields(first(signal, 1)), 2, b'').decode()
                                offer_ufrags.add(tuple(re.findall(r'^a=ice-ufrag:(\S+)', sdp, re.MULTILINE)))
                            if 12 in signal:
                                sync_states += 1
                        await destination.send(data)

                tasks = [asyncio.create_task(pump(client, upstream)), asyncio.create_task(pump(upstream, client))]
                try:
                    finished, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
                    for task in finished:
                        task.result()
                finally:
                    for task in tasks + releases:
                        task.cancel()
                    await asyncio.gather(*tasks, *releases, return_exceptions=True)
        except ConnectionClosed:
            pass  # expected during disconnect and injected network cuts
        except Exception as exc:
            proxy_errors.append(type(exc).__name__)
        finally:
            connections.discard(client)

    async def cuts():
        nonlocal reject_resume, server
        if args.scenario in ('operations', 'video-stream', 'audio-stream', 'video-source', 'audio-source'):
            marker = {'operations': 'operations-rejoin', 'video-stream': 'stream-rejoin', 'audio-stream': 'audio-rejoin', 'video-source': 'source-rejoin', 'audio-source': 'pcm-source-rejoin'}[args.scenario]
            while not (root / marker).exists():
                await asyncio.sleep(.02)
            for client in tuple(connections):
                await client.send(bytes.fromhex('420410061802'))
            return
        if args.scenario in ('rejoin', 'restart', 'region', 'resume-disabled'):
            for marker, action in [('rejoin-1', 'rejoin'), ('rejoin-2', 'reject-resume'), ('terminal', 'terminal')]:
                while not (root / marker).exists():
                    await asyncio.sleep(.02)
                if args.scenario == 'restart' and marker == 'rejoin-1':
                    server.kill()  # only the isolated test server; no graceful Leave
                    await asyncio.to_thread(server.wait, 10)
                    server = subprocess.Popen([args.server, '--config', str(config)], stdout=log, stderr=log, creationflags=flags)
                    continue
                for client in tuple(connections):
                    if args.scenario == 'resume-disabled' and action != 'terminal':
                        await client.close(code=1012, reason='test resume disabled')
                    elif action == 'reject-resume':
                        reject_resume = True
                        await client.close(code=1012, reason='test session lost')
                    else:
                        # terminal = PARTICIPANT_REMOVED despite action RECONNECT.
                        leave = bytes.fromhex('10041802' if action == 'terminal' else '10061802')
                        if args.scenario == 'region' and marker == 'rejoin-1':
                            url = f'http://127.0.0.1:{proxy_port}/alternate'.encode()
                            leave += wire_bytes(4, wire_bytes(1, wire_bytes(2, url)))
                        await client.send(wire_bytes(8, leave))
            return
        for round_number in range(1, args.rounds + 2):
            marker = root / f'cut-{round_number}'
            while not marker.exists():
                await asyncio.sleep(.02)
            for client in tuple(connections):
                await client.close(code=1012, reason='test signal interruption')

    with (root / 'server.log').open('w') as log:
        server = subprocess.Popen([args.server, '--config', str(config)], stdout=log, stderr=log, creationflags=flags)
        try:
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                if server.poll() is not None:
                    raise RuntimeError(f'Server exited; see {root}')
                try:
                    # Listening precedes routing readiness on some server starts.
                    async with connect(f'ws://127.0.0.1:{port}/rtc?protocol=12&sdk=cpp&version=0.3.0',
                            additional_headers={'Authorization': 'Bearer ' + readiness_token()},
                            proxy=None, compression=None, open_timeout=2) as probe:
                        response = fields(await asyncio.wait_for(probe.recv(), 2))
                        if 1 not in response or 2 not in fields(first(response, 1)):
                            raise RuntimeError('Readiness probe did not receive JoinResponse')
                    break
                except Exception:
                    await asyncio.sleep(.1)
            else:
                raise TimeoutError('Server startup timeout')
            async with serve(proxy, '127.0.0.1', proxy_port, compression=None):
                cutter = asyncio.create_task(cuts())
                try:
                    with (root / 'client.stdout.log').open('w') as out, (root / 'client.stderr.log').open('w') as err:
                        process = await asyncio.create_subprocess_exec(str(Path(args.client).resolve()),
                            f'http://127.0.0.1:{port}', f'http://127.0.0.1:{proxy_port}', str(root),
                            *([str(args.rounds)] if args.scenario == 'resume' else []),
                            *(['--operations'] if args.scenario == 'operations' else []),
                            *(['--video-stream'] if args.scenario == 'video-stream' else []),
                            *(['--audio-stream'] if args.scenario == 'audio-stream' else []),
                            *(['--video-source-rgba' if args.rgba else '--video-source'] if args.scenario == 'video-source' else []),
                            *(['--audio-source'] if args.scenario == 'audio-source' else []),
                            stdout=out, stderr=err, creationflags=flags,
                            env=dict(os.environ, LIVEKIT_TEST_PROTOCOL=str(args.protocol)))
                        try:
                            code = await asyncio.wait_for(process.wait(), timeout=max(75, 30 + args.rounds * 4))
                        finally:
                            if process.returncode is None:
                                process.kill()
                                await process.wait()
                    if code:
                        (root / 'proxy-diagnostics.json').write_text(json.dumps({'errors': proxy_errors,
                            'metadata': operation_metadata, 'acknowledgements': real_acknowledgements}, indent=2))
                        raise RuntimeError(f'Client failed: {root}')
                finally:
                    cutter.cancel()
                    with contextlib.suppress(asyncio.CancelledError):
                        await cutter
            resume_requests = [q for q in requests if q.get('reconnect') == ['1']]
            if args.scenario == 'restart':
                assert len(resume_requests) >= 2, requests
            elif args.scenario in ('resume-disabled', 'operations', 'video-stream', 'audio-stream', 'video-source', 'audio-source'):
                assert len(resume_requests) == 0, requests
            else:
                assert len(resume_requests) == (1 if args.scenario in ('rejoin', 'region') else args.rounds), requests
            assert all(q.get('sid', [''])[0].startswith('PA_') for q in resume_requests)
            assert all(q.get('version') == ['0.3.0'] for q in requests)
            expected_protocol = 15 if args.scenario in ('operations', 'video-stream', 'audio-stream', 'video-source', 'audio-source') else args.protocol
            assert all(q.get('protocol') == [str(expected_protocol)] for q in requests)
            if args.scenario == 'resume':
                assert sync_states == args.rounds, sync_states
                assert len(offer_ufrags) >= args.rounds + 1, 'Publisher ICE credentials did not change after resume'
            if args.scenario == 'region':
                assert paths[0] == '/rtc' and all(path == '/alternate/rtc' for path in paths[1:]), paths
            report = json.loads((root / 'client.stdout.log').read_text())
            assert not proxy_errors, proxy_errors
            report.update(resumeRequests=len(resume_requests), proxyErrors=proxy_errors,
                          signalProtocol=expected_protocol,
                          distinctPublisherICEOffers=len(offer_ufrags), syncStates=sync_states,
                          clientSHA256=hashlib.sha256(Path(args.client).read_bytes()).hexdigest(),
                          serverSHA256=hashlib.sha256(Path(args.server).read_bytes()).hexdigest())
            if args.scenario == 'region':
                report['trustedRegionRejoin'] = True
            if args.scenario == 'resume-disabled':
                report['disabledResumeHonored'] = True
            if args.scenario == 'operations':
                assert 'must-not-send' not in operation_metadata
                assert any(ack == {'metadata': 'ack-ok', 'reason': 0} for ack in real_acknowledgements), real_acknowledgements
                report['realAcknowledgements'] = real_acknowledgements
                report['preCancelledRequestNotSent'] = True
            (root / 'result.json').write_text(json.dumps(report, indent=2))
            print(json.dumps(report), root)
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
    parser.add_argument('--scenario', choices=['resume', 'rejoin', 'restart', 'region', 'resume-disabled', 'operations', 'video-stream', 'audio-stream', 'video-source', 'audio-source'], default='resume')
    parser.add_argument('--rounds', type=int, default=2)
    parser.add_argument('--rgba', action='store_true', help='Exercise rotated RGBA input in video-source scenario')
    parser.add_argument('--protocol', type=int, choices=[12, 15], default=12, help='Signal protocol for resume scenario')
    args = parser.parse_args()
    if args.rgba and args.scenario != 'video-source':
        parser.error('--rgba requires --scenario video-source')
    if args.protocol != 12 and args.scenario != 'resume':
        parser.error('--protocol applies to the resume scenario; operations always uses 15')
    asyncio.run(run(args))
