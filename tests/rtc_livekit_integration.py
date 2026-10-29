"""Exercise encoded RoomSession against an isolated, explicitly supplied SFU."""
import argparse
import base64
import hashlib
import hmac
import json
import os
import re
from pathlib import Path
import socket
import subprocess
import time
import tempfile


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def token(identity, secret, kind="standard"):
    def encode(value):
        return base64.urlsafe_b64encode(json.dumps(value, separators=(',', ':')).encode()).rstrip(b'=')
    now = int(time.time())
    message = encode({'alg': 'HS256', 'typ': 'JWT'}) + b'.' + encode({
        'iss': 'rtctest', 'sub': identity, 'nbf': now - 10, 'exp': now + 300, 'kind': kind,
        'video': {'roomJoin': True, 'room': 'encoded-backend', 'canPublish': True, 'canSubscribe': True,
                  'canUpdateOwnMetadata': identity == 'publisher'}})
    signature = base64.urlsafe_b64encode(hmac.new(secret.encode(), message, hashlib.sha256).digest()).rstrip(b'=')
    return (message + b'.' + signature).decode()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--client', required=True)
    parser.add_argument('--server', required=True)
    parser.add_argument('--audio-source', help='Run audio_stream as --client with this audio_source example')
    parser.add_argument('--encrypted', action='store_true')
    parser.add_argument('--media-encrypted', action='store_true')
    parser.add_argument('--publisher-kind', choices=['standard', 'agent'], default='standard')
    args = parser.parse_args()
    if args.audio_source and (args.encrypted or args.media_encrypted):
        parser.error('The PCM examples do not configure encryption; use the audio graph integration client')
    parent = Path('out/rtc-livekit-integration').resolve()
    parent.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix=time.strftime('%Y%m%d-%H%M%S-'), dir=parent))
    port, rtc, udp = free_port(), free_port(), free_port(socket.SOCK_DGRAM)
    while rtc == port:
        rtc = free_port()
    secret = 'encoded-backend-integration-secret-32-characters'
    config = root / 'server.yaml'
    config.write_text(f'''port: {port}
bind_addresses: [127.0.0.1]
rtc:
  tcp_port: {rtc}
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
    env = dict(os.environ, LIVEKIT_URL=f'http://127.0.0.1:{port}',
               LIVEKIT_PUBLISH_TOKEN=token('publisher', secret, args.publisher_kind),
               LIVEKIT_SUBSCRIBE_TOKEN=token('subscriber', secret),
               LIVEKIT_DATA_ENCRYPTION='1' if args.encrypted else '0',
               LIVEKIT_MEDIA_ENCRYPTION='1' if args.media_encrypted else '0')
    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    print(root, flush=True)
    with open(root / 'server.log', 'w') as log:
        server = subprocess.Popen([str(Path(args.server).resolve()), '--config', str(config)],
                                  stdout=log, stderr=log, creationflags=flags)
        try:
            deadline = time.monotonic() + 20
            while True:
                if server.poll() is not None:
                    raise RuntimeError(f'Server exited; see {root}')
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=.5):
                        break
                except OSError:
                    if time.monotonic() > deadline:
                        raise
                    time.sleep(.1)
            if args.audio_source:
                report = audio_examples(args, env, flags, root)
            else:
                with open(root / 'client.stdout.log', 'w') as out, open(root / 'client.stderr.log', 'w') as err:
                    result = subprocess.run([str(Path(args.client).resolve())], env=env, stdout=out,
                                            stderr=err, timeout=100, creationflags=flags)
                if result.returncode:
                    raise RuntimeError(f'Client failed ({result.returncode}); see {root}')
                report = json.loads((root / 'client.stdout.log').read_text())
            (root / 'result.json').write_text(json.dumps(report, indent=2))
            print(json.dumps(report))
        finally:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()


def audio_examples(args, env, flags, root):
    with open(root / 'client.stdout.log', 'w') as out, open(root / 'client.stderr.log', 'w') as err:
        receiver = subprocess.Popen([str(Path(args.client).resolve())],
                                    env=dict(env, LIVEKIT_TOKEN=env['LIVEKIT_SUBSCRIBE_TOKEN']),
                                    stdout=out, stderr=err, creationflags=flags)
        try:
            with open(root / 'source.stdout.log', 'w') as src_out, open(root / 'source.stderr.log', 'w') as src_err:
                sender = subprocess.run([str(Path(args.audio_source).resolve()), env['LIVEKIT_URL'],
                                         env['LIVEKIT_PUBLISH_TOKEN']], env=env, stdout=src_out,
                                        stderr=src_err, timeout=60, creationflags=flags)
            if sender.returncode or receiver.wait(timeout=40):
                raise RuntimeError(f'PCM example failed; see {root}')
        finally:
            if receiver.poll() is None:
                receiver.kill()
            receiver.wait()
    samples = sum(map(int, re.findall(r'samples/channel=(\d+)', (root / 'client.stdout.log').read_text())))
    packets = re.search(r'Encoded packets: (\d+)', (root / 'source.stdout.log').read_text())
    if samples < 48000 or not packets or int(packets[1]) < 100:
        raise RuntimeError(f'PCM example did not make sufficient progress; see {root}')
    return {'audioExamples': True, 'samplesPerChannel': samples, 'encodedPackets': int(packets[1])}


if __name__ == '__main__':
    main()
