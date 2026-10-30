"""Native SDK SRTP sender -> Go LiveKit -> ordinary native subscriber.

The native fixture injects upstream ciphertext loss and rejects RTX negotiation.
This runner handles only protobuf signaling; it never edits SDP or RTP packets.
Requires websockets and generated livekit_pb2 from protocol/livekit.proto.
"""
import argparse
import asyncio
import contextlib
import hashlib
import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import time

from websockets.asyncio.client import connect
from rtc_livekit_integration import free_port, token


async def run(args):
    sys.path.insert(0, str(Path(args.proto_dir).resolve()))
    import livekit_pb2 as pb

    root = Path(args.output).resolve()
    root.mkdir(parents=True, exist_ok=True)
    direction = 'downstream' if args.downstream else 'upstream'
    case = 'fallback' if args.receiver_no_fec else 'negative' if args.discard_fec else 'recovery'
    directory = Path(tempfile.mkdtemp(prefix=f'{args.codec}-{direction}-{case}-', dir=root))
    print(directory, flush=True)
    port, tcp, udp = free_port(), free_port(), free_port(socket.SOCK_DGRAM)
    secret = 'encoded-backend-integration-secret-32-characters'
    config = directory / 'server.yaml'
    config.write_text(f'''port: {port}
bind_addresses: [127.0.0.1]
rtc:
  tcp_port: {tcp}
  udp_port: {udp}
  use_external_ip: false
  node_ip: 127.0.0.1
  ips:
    includes: [127.0.0.1/32]
  enable_loopback_candidate: true
keys:
  rtctest: {secret}
logging:
  level: info
''')
    report = {'ok': False, 'codec': args.codec, 'discardFec': args.discard_fec,
              'direction': direction, 'receiverFec': not args.receiver_no_fec,
              'serverSha256': hashlib.sha256(Path(args.server).read_bytes()).hexdigest(),
              'clientSha256': hashlib.sha256(Path(args.client).read_bytes()).hexdigest()}
    flags = 0x08000000 if os.name == 'nt' else 0  # CREATE_NO_WINDOW
    tasks, sockets, descriptions = [], [], []
    server = client = None
    events = {key: asyncio.Event() for key in ('ready', 'published', 'negotiated', 'tick_done', 'result')}
    joined = [asyncio.Event(), asyncio.Event()]

    def observe(sdp, peer, kind):
        # Retain media negotiation metadata only, never ICE credentials or tokens.
        descriptions.append({'peer': peer, 'type': kind,
                             'lines': [line for line in sdp.splitlines() if line.startswith(
                                 ('m=', 'a=rtpmap:', 'a=fmtp:', 'a=ssrc-group:'))]})

    async def command(line):
        client.stdin.write((line + '\n').encode())
        await client.stdin.drain()

    async def signaling(ws, index):
        async for data in ws:
            msg = pb.SignalResponse()
            msg.ParseFromString(data)
            if msg.HasField('join'):
                joined[index].set()
            elif msg.HasField('track_published'):
                events['published'].set()
            elif msg.HasField('offer'):
                peer = 1 if index == 0 else 2
                observe(msg.offer.sdp, peer, 'offer')
                await command(f'remote {peer} offer ' + msg.offer.sdp.encode().hex())
            elif msg.HasField('answer'):
                observe(msg.answer.sdp, 0, 'answer')
                await command('remote 0 answer ' + msg.answer.sdp.encode().hex())
            elif msg.HasField('trickle') and not msg.trickle.final:
                candidate = json.loads(msg.trickle.candidateInit)
                if not candidate.get('candidate'):
                    continue
                peer = (1 if index == 0 else 2) if msg.trickle.target == pb.SUBSCRIBER else 0
                await command(f"ice {peer} {candidate.get('sdpMLineIndex', 0)} {candidate.get('sdpMid', '0')} "
                              + candidate['candidate'].encode().hex())
            elif msg.HasField('leave'):
                raise RuntimeError(f'Server closed participant {index}: reason {msg.leave.reason}')

    async def native_output():
        while line := await client.stdout.readline():
            text = line.decode().strip()
            if text in events:
                events[text].set()
            elif text.startswith(('offer ', 'answer ')):
                kind, peer, value = text.split(' ', 2)
                peer = int(peer)
                sdp = bytes.fromhex(value).decode()
                observe(sdp, peer, kind)
                req = pb.SignalRequest()
                field = getattr(req, kind)
                field.type, field.sdp = kind, sdp
                await sockets[0 if peer < 2 else 1].send(req.SerializeToString())
            elif text.startswith('result '):
                report['media'] = json.loads(text[7:])
                events['result'].set()
            else:
                raise RuntimeError('Unexpected native output: ' + text)

    async def wait(event, timeout=30):
        pending = asyncio.create_task(event.wait())
        try:
            done, _ = await asyncio.wait([pending, *tasks], timeout=timeout, return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                if task is not pending:
                    task.result()
                    raise RuntimeError('Test signaling/native output ended early')
            if pending not in done:
                raise TimeoutError('Test event timed out')
        finally:
            pending.cancel()
            await asyncio.gather(pending, return_exceptions=True)

    with open(directory / 'server.log', 'w') as server_log, open(directory / 'client.stderr.log', 'w') as client_log:
        try:
            server = await asyncio.create_subprocess_exec(str(Path(args.server).resolve()), '--config', str(config),
                                                         stdout=server_log, stderr=server_log, creationflags=flags)
            for _ in range(200):
                try:
                    reader, writer = await asyncio.open_connection('127.0.0.1', port)
                    writer.close()
                    await writer.wait_closed()
                    break
                except OSError:
                    if server.returncode is not None:
                        raise RuntimeError('Go SFU exited on startup')
                    await asyncio.sleep(.1)
            else:
                raise TimeoutError('Go SFU startup timed out')
            command_args = [str(Path(args.client).resolve()), '--sfu', args.codec]
            if args.discard_fec:
                command_args.append('--discard-fec')
            if args.downstream:
                command_args.append('--downstream')
            if args.receiver_no_fec:
                command_args.append('--receiver-no-fec')
            client = await asyncio.create_subprocess_exec(*command_args, stdin=asyncio.subprocess.PIPE,
                                                         stdout=asyncio.subprocess.PIPE, stderr=client_log,
                                                         creationflags=flags, limit=1024*1024)
            tasks.append(asyncio.create_task(native_output()))
            await wait(events['ready'])
            for index, identity in enumerate(('publisher', 'subscriber')):
                ws = await connect(f'ws://127.0.0.1:{port}/rtc?protocol=15&auto_subscribe=1&sdk=cpp&version=0.1',
                                   additional_headers={'Authorization': 'Bearer ' + token(identity, secret)},
                                   compression=None, proxy=None, max_size=4*1024*1024)
                sockets.append(ws)
                tasks.append(asyncio.create_task(signaling(ws, index)))
                await wait(joined[index])
            request = pb.SignalRequest()
            request.subscription_permission.all_participants = True
            await sockets[0].send(request.SerializeToString())
            request = pb.SignalRequest()
            track = request.add_track
            track.cid = track.name = 'video'
            track.type, track.source, track.width, track.height = pb.VIDEO, pb.CAMERA, 64, 48
            track.simulcast_codecs.add(codec=args.codec, cid='video')
            track.layers.add(quality=2, width=64, height=48, bitrate=500000)
            await sockets[0].send(request.SerializeToString())
            await wait(events['published'])
            await command('offer')
            await wait(events['negotiated'])
            await asyncio.sleep(.5)
            for i in range(200):
                events['tick_done'].clear()
                await command(f'tick {i}')
                await wait(events['tick_done'])
            await command('finish')
            # Native stdout can close immediately after reporting; await the
            # result directly here and check the process exit separately.
            await asyncio.wait_for(events['result'].wait(), 10)
            status = await asyncio.wait_for(client.wait(), 10)
            if status:
                raise RuntimeError(f'Native recovery assertions failed: exit {status}')
            report['ok'] = True
        except Exception as error:
            report['error'] = str(error)
        finally:
            for ws in sockets:
                with contextlib.suppress(Exception):
                    leave = pb.SignalRequest()
                    leave.leave.SetInParent()
                    await ws.send(leave.SerializeToString())
                await ws.close()
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            if client and client.returncode is None:
                client.kill()
                await client.wait()
            if server:
                await asyncio.sleep(.3)
                if server.returncode is None:
                    server.terminate()
                await asyncio.wait_for(server.wait(), 10)
    stats, downstream_stats = [], []
    for line in (directory / 'server.log').read_text().splitlines():
        if 'upstream FlexFEC statistics' in line:
            stats.append(json.loads(line[line.index('{'):]))
        if 'downstream FlexFEC statistics' in line:
            downstream_stats.append(json.loads(line[line.index('{'):]))
    report['sfuStats'] = [{key: s[key] for key in ('participant', 'repairPackets', 'recoveredPackets', 'rejectedPackets')}
                          for s in stats]
    report['sfuDownstreamStats'] = [{key: s[key] for key in ('ssrc', 'repairSSRC', 'protectedPackets', 'repairPackets', 'repairBytes', 'oversizedPackets')}
                                   for s in downstream_stats]
    if report['ok'] and not args.downstream:
        publisher = next((s for s in stats if s['participant'] == 'publisher'), None)
        if publisher is None or publisher['recoveredPackets'] != report['media']['recovered'] or publisher['rejectedPackets']:
            report.update(ok=False, error='SFU recovery counters do not match subscriber evidence')
    if report['ok'] and args.downstream:
        answer = [d for d in descriptions if d['peer'] == 2 and d['type'] == 'answer'
                  and any(line.startswith('m=video ') for line in d['lines'])]
        negotiated = bool(answer) and all(any('flexfec-03/90000' in line for line in d['lines']) for d in answer)
        if negotiated != (not args.receiver_no_fec):
            report.update(ok=False, error='Downstream SDP does not match requested FEC capability')
        if args.receiver_no_fec:
            if any(s['repairPackets'] for s in downstream_stats):
                report.update(ok=False, error='SFU sent FEC to an unsupported subscriber')
        elif not downstream_stats or sum(s['repairPackets'] for s in downstream_stats) < report['media']['fec']:
            report.update(ok=False, error='Missing SFU downstream repair transmission evidence')
    (directory / 'sdp-summary.json').write_text(json.dumps(descriptions, indent=2))
    (directory / 'result.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report), flush=True)
    return 0 if report['ok'] else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', required=True)
    parser.add_argument('--server', required=True)
    parser.add_argument('--proto-dir', required=True)
    parser.add_argument('--codec', choices=['vp8', 'h264', 'h265'], required=True)
    parser.add_argument('--discard-fec', action='store_true')
    parser.add_argument('--downstream', action='store_true', help='Inject loss after the SFU at the subscriber SRTP socket')
    parser.add_argument('--receiver-no-fec', action='store_true', help='Exercise downstream legacy fallback')
    parser.add_argument('--output', default='out/flexfec-go-sfu-20261030')
    sys.exit(asyncio.run(run(parser.parse_args())))
