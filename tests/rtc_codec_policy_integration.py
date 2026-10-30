"""Four native SDK clients through an isolated SFU; generated video, no devices."""
import argparse
import base64
import hashlib
import hmac
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

from rtc_livekit_integration import free_port


def token(identity, secret, h265):
    def encode(value):
        return base64.urlsafe_b64encode(json.dumps(value).encode()).rstrip(b'=')
    now = int(time.time())
    payload = {'iss': 'codec-test', 'sub': identity, 'nbf': now - 10, 'exp': now + 300,
               'attributes': {'av.video.decode_codecs': 'h264,h265' if h265 else 'h264'},
               'video': {'roomJoin': True, 'room': 'codec-policy', 'canPublish': True,
                         'canSubscribe': True, 'canUpdateOwnMetadata': True}}
    message = encode({'alg': 'HS256', 'typ': 'JWT'}) + b'.' + encode(payload)
    signature = base64.urlsafe_b64encode(hmac.new(secret.encode(), message, hashlib.sha256).digest())
    return (message + b'.' + signature.rstrip(b'=')).decode()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', required=True)
    parser.add_argument('--client', required=True)
    parser.add_argument('--server-kind', choices=['go', 'cpp'], default='go')
    args = parser.parse_args()
    parent = Path('out/rtc-codec-policy-integration').resolve()
    parent.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix=time.strftime('%Y%m%d-%H%M%S-'), dir=parent))
    port, tcp, udp = free_port(), free_port(), free_port(socket.SOCK_DGRAM)
    while port == tcp:
        tcp = free_port()
    secret = 'codec-policy-integration-secret-32-characters'
    config = root / 'server.yaml'
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
  codec-test: {secret}
logging:
  level: info
''')
    if args.server_kind == 'cpp':
        config = root / 'server.json'
        config.write_text(json.dumps({'bind': '127.0.0.1', 'port': port,
                                      'keys': {'codec-test': secret}}))
    env = dict(os.environ, LIVEKIT_URL=f'ws://127.0.0.1:{port}')
    for name, h265 in [('PUBLISHER_A', True), ('PUBLISHER_B', True),
                       ('H264_VIEWER', False), ('LATE_PUBLISHER', True)]:
        env[f'LIVEKIT_CODEC_{name}'] = token(name.lower(), secret, h265)
    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    print(root, flush=True)
    with (root / 'server.log').open('w') as log:
        server = subprocess.Popen([str(Path(args.server).resolve()), '--config', str(config)],
                                  stdout=log, stderr=log, creationflags=flags)
        try:
            deadline = time.monotonic() + 20
            while True:
                if server.poll() is not None:
                    raise RuntimeError(f'SFU exited; see {root}')
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=.5):
                        break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(.1)
            with (root / 'client.stdout.log').open('w') as out, (root / 'client.stderr.log').open('w') as err:
                result = subprocess.run([str(Path(args.client).resolve())], env=env,
                                        stdout=out, stderr=err, timeout=150, creationflags=flags)
            report = {'ok': result.returncode == 0, 'exitCode': result.returncode,
                      'serverKind': args.server_kind}
            if report['ok']:
                report.update(json.loads((root / 'client.stdout.log').read_text()))
            for name, path in [('server', args.server), ('client', args.client)]:
                report[f'{name}Binary'] = str(Path(path).resolve())
                report[f'{name}Sha256'] = hashlib.sha256(Path(path).read_bytes()).hexdigest()
            (root / 'result.json').write_text(json.dumps(report, indent=2))
            print(json.dumps(report))
            if not report['ok']:
                raise RuntimeError(f'Codec policy failed; see {root}')
        finally:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()


if __name__ == '__main__':
    main()
