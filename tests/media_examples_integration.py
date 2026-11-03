"""Run migrated SDK examples against an isolated SFU without native devices."""
import argparse
import base64
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def token(room, identity):
    def encode(value):
        return base64.urlsafe_b64encode(json.dumps(value).encode()).rstrip(b'=')
    body = b'.'.join((encode({'alg': 'HS256', 'typ': 'JWT'}), encode({
        'iss': 'examples', 'sub': identity, 'nbf': int(time.time()) - 5,
        'exp': int(time.time()) + 300,
        'video': {'roomJoin': True, 'room': room, 'canPublish': True,
                  'canSubscribe': True, 'canPublishData': True}})))
    signature = base64.urlsafe_b64encode(hmac.new(SECRET.encode(), body, hashlib.sha256).digest()).rstrip(b'=')
    return (body + b'.' + signature).decode()


SECRET = 'isolated-sdk-example-test-secret'
FLAGS = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--server', type=Path, required=True)
parser.add_argument('--bin', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
root = args.output.resolve()
root.mkdir(parents=True, exist_ok=False)
port, tcp, udp = free_port(), free_port(), free_port(socket.SOCK_DGRAM)
url = f'ws://127.0.0.1:{port}'
config = root / 'server.yaml'
config.write_text(f'port: {port}\nbind_addresses: [127.0.0.1]\nrtc:\n'
                  f'  tcp_port: {tcp}\n  udp_port: {udp}\n  use_external_ip: false\n'
                  '  node_ip: 127.0.0.1\n  enable_loopback_candidate: true\n'
                  f'keys:\n  examples: {SECRET}\nlogging:\n  level: warn\n', encoding='utf-8')
report = {'passed': False, 'native_devices': False, 'cases': []}


def run_pair(name, publisher, receiver, positive):
    processes = []
    logs = []
    try:
        for identity, (binary, arguments) in [('receiver', receiver), ('publisher', publisher)]:
            credential = token(name, identity)
            arguments = [item.replace('{url}', url).replace('{token}', credential) for item in arguments]
            log = (root / f'{name}-{identity}.log').open('w', encoding='utf-8')
            logs.append(log)
            processes.append(subprocess.Popen([str(args.bin.resolve() / (binary + ('.exe' if os.name == 'nt' else ''))), *arguments],
                env=dict(os.environ, LIVEKIT_URL=url, LIVEKIT_TOKEN=credential), stdout=log, stderr=log,
                creationflags=FLAGS))
        codes = [process.wait(timeout=45) for process in processes]
        for log in logs:
            log.flush()
        output = (root / f'{name}-receiver.log').read_text(encoding='utf-8', errors='replace')
        matches = re.findall(positive, output)
        ok = codes == [0, 0] and any(int(value) > 0 for value in matches)
        report['cases'].append({'name': name, 'exit_codes': codes, 'received_media': ok})
        if not ok:
            raise RuntimeError(f'{name} failed; see {root}')
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
            process.wait()
        for log in logs:
            log.close()


with (root / 'server.log').open('w', encoding='utf-8') as log:
    server = subprocess.Popen([str(args.server.resolve()), '--config', str(config)], stdout=log, stderr=log, creationflags=FLAGS)
    try:
        deadline = time.monotonic() + 20
        while True:
            if server.poll() is not None:
                raise RuntimeError('SFU exited')
            try:
                with socket.create_connection(('127.0.0.1', port), timeout=.5):
                    break
            except OSError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.1)
        run_pair('video', ('livekit_video_source_example', ['{url}', '{token}']),
                 ('livekit_video_stream_example', []), r'frames=(\d+)')
        run_pair('audio', ('livekit_audio_source_example', ['{url}', '{token}']),
                 ('livekit_audio_stream_example', []), r'samples/channel=(\d+)')
        run_pair('encoded', ('livekit_sdk_example', ['{url}', 'publish', '5']),
                 ('livekit_sdk_example', ['{url}', 'subscribe', '6']), r'received (\d+) encoded frames')
        run_pair('basic', ('livekit_basic_room_encoded', []),
                 ('livekit_sdk_example', ['{url}', 'subscribe', '6']), r'received (\d+) encoded frames')
        run_pair('automatic', ('livekit_automatic_session_example', ['{url}', '{token}']),
                 ('livekit_sdk_example', ['{url}', 'subscribe', '6']), r'received (\d+) encoded frames')
        report['passed'] = True
    finally:
        server.terminate()
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
        config.unlink(missing_ok=True)
        (root / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps(report))
