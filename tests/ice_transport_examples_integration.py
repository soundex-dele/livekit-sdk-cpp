"""Verify decoded example media and selected ICE protocols on one Go SFU instance."""
import argparse
import base64
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import secrets
import socket
import subprocess
import time


def free_port(kind):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def token(secret, room, identity):
    def encode(value):
        return base64.urlsafe_b64encode(json.dumps(value).encode()).rstrip(b'=')
    now = int(time.time())
    body = b'.'.join((encode({'alg': 'HS256', 'typ': 'JWT'}), encode({
        'iss': 'ice-examples', 'sub': identity, 'nbf': now - 5, 'exp': now + 300,
        'video': {'roomJoin': True, 'room': room, 'canPublish': True,
                  'canSubscribe': True, 'canPublishData': True}})))
    signature = base64.urlsafe_b64encode(hmac.new(secret.encode(), body, hashlib.sha256).digest())
    return (body + b'.' + signature.rstrip(b'=')).decode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--bin', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    signal = free_port(socket.SOCK_STREAM)
    tcp = free_port(socket.SOCK_STREAM)
    while tcp == signal:
        tcp = free_port(socket.SOCK_STREAM)
    udp = free_port(socket.SOCK_DGRAM)
    secret = secrets.token_hex(32)
    config = root / 'server.yaml'
    config.write_text(f'''port: {signal}
bind_addresses: [127.0.0.1]
rtc:
  tcp_port: {tcp}
  udp_port: {udp}
  use_external_ip: false
  enable_loopback_candidate: true
  node_ip: 127.0.0.1
  ips:
    includes: [127.0.0.1/32]
keys:
  ice-examples: {secret}
logging:
  level: debug
  json: true
''', encoding='utf-8')
    hidden = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    suffix = '.exe' if os.name == 'nt' else ''
    report = {'passed': False, 'native_devices': False, 'single_server_instance': True,
              'ports': {'signal': signal, 'ice_tcp': tcp, 'ice_udp': udp}, 'cases': []}
    clients, logs = [], []
    server = None
    try:
        with (root / 'server.log').open('w', encoding='utf-8') as server_log:
            server = subprocess.Popen([str(args.server.resolve()), '--config', str(config)],
                                      stdout=server_log, stderr=subprocess.STDOUT, creationflags=hidden)
            report['server_pid'] = server.pid
            deadline = time.monotonic() + 30
            while True:
                if server.poll() is not None:
                    raise RuntimeError('SFU exited; inspect server.log')
                try:
                    with socket.create_connection(('127.0.0.1', signal), timeout=.3):
                        break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise RuntimeError('SFU readiness timeout')
                    time.sleep(.1)
            for media in ('video', 'audio'):
                batch = []
                for pub_mode, sub_mode in (('udp', 'udp'), ('tcp', 'tcp'),
                                           ('udp', 'tcp'), ('tcp', 'udp')):
                    name = f'{media}-{pub_mode}-to-{sub_mode}'
                    case = {'name': name, 'publisher_protocol': pub_mode,
                            'subscriber_protocol': sub_mode, 'clients': []}
                    report['cases'].append(case)
                    for role, mode in (('subscriber', sub_mode), ('publisher', pub_mode)):
                        identity = f'{name}-{role}'
                        credential = token(secret, name, identity)
                        binary = f'livekit_{media}_{"source" if role == "publisher" else "stream"}_example'
                        arguments = ([f'ws://127.0.0.1:{signal}', credential]
                                     if role == 'publisher' else [])
                        log = (root / f'{identity}.log').open('w', encoding='utf-8')
                        logs.append(log)
                        env = dict(os.environ, LIVEKIT_URL=f'ws://127.0.0.1:{signal}',
                                   LIVEKIT_TOKEN=credential, LIVEKIT_MEDIA_TRANSPORT=mode)
                        process = subprocess.Popen([str(args.bin.resolve() / (binary + suffix)), *arguments],
                                                   stdout=log, stderr=subprocess.STDOUT, env=env,
                                                   creationflags=hidden)
                        clients.append(process)
                        entry = {'identity': identity, 'role': role, 'protocol': mode}
                        case['clients'].append(entry)
                        batch.append((process, log, entry))
                deadline = time.monotonic() + 55
                for process, log, entry in batch:
                    entry['exit_code'] = process.wait(timeout=max(.1, deadline - time.monotonic()))
                    log.flush()
                if server.poll() is not None:
                    raise RuntimeError('SFU exited during media tests')
            # Keep the same SFU alive until all media cases have completed.
            server.terminate()
            server.wait(timeout=15)
        pairs = []
        for line in (root / 'server.log').read_text(encoding='utf-8', errors='replace').splitlines():
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            if event.get('msg') == 'selected ICE candidate pair changed':
                pairs.append({key: event.get(key) for key in
                              ('ts', 'room', 'roomID', 'participant', 'transport', 'pair')})
        (root / 'selected-pairs.json').write_text(json.dumps(pairs, indent=2), encoding='utf-8')
        for case in report['cases']:
            for entry in case['clients']:
                selected = [p for p in pairs if p['participant'] == entry['identity']]
                needed = 'PUBLISHER' if entry['role'] == 'publisher' else 'SUBSCRIBER'
                entry['selected_pairs'] = selected
                entry['protocol_verified'] = bool(selected) and any(p['transport'] == needed for p in selected) and all(
                    p['pair']['localProtocol'] == entry['protocol'] and
                    p['pair']['remoteProtocol'] == entry['protocol'] and
                    p['pair']['localPort'] == (tcp if entry['protocol'] == 'tcp' else udp)
                    for p in selected)
            subscriber = case['clients'][0]
            output = (root / f'{subscriber["identity"]}.log').read_text(encoding='utf-8', errors='replace')
            pattern = r'frames=(\d+)' if case['name'].startswith('video') else r'samples/channel=(\d+)'
            case['decoded_count'] = sum(map(int, re.findall(pattern, output)))
            case['passed'] = case['decoded_count'] > 0 and all(
                e['exit_code'] == 0 and e['protocol_verified'] for e in case['clients'])
        report['passed'] = all(case['passed'] for case in report['cases'])
    finally:
        for process in clients + ([server] if server else []):
            if process.poll() is None:
                process.kill()
            process.wait()
        for log in logs:
            log.close()
        config.unlink(missing_ok=True)
        (root / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({'passed': report['passed'], 'server_pid': report['server_pid'],
                      'ports': report['ports'], 'cases': [
                          {key: case[key] for key in ('name', 'passed', 'decoded_count')}
                          for case in report['cases']]}))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
