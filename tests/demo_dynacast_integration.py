"""Headless real Go SFU tests for the conference demo's aggregate source control."""
import argparse
from datetime import datetime, timezone, timedelta
import json
import os
from pathlib import Path

from uav_requirements import SDK, WORKSPACE, digest, git_info, run_case


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--server', type=Path, required=True, help='Go SFU binary built from D:/workspace/livekit')
    p.add_argument('--client', type=Path, default=SDK / 'out/windows-x64/Release/livekit_demo_dynacast_integration.exe')
    p.add_argument('--output', type=Path)
    args = p.parse_args()
    # Exercise the demo's built-in test credentials against the matching fixture,
    # independently of credentials used by the developer's interactive shell.
    os.environ.pop('LIVEKIT_API_KEY', None)
    os.environ.pop('LIVEKIT_API_SECRET', None)
    now = datetime.now(timezone(timedelta(hours=8)))
    root = (args.output or WORKSPACE / 'out/demo-dynacast' / now.strftime('%Y%m%d-%H%M%S')).resolve()
    root.mkdir(parents=True, exist_ok=False)
    report = {'time': now.isoformat(), 'server': str(args.server.resolve()),
              'server_sha256': digest(args.server), 'client_sha256': digest(args.client),
              'sdk': git_info(SDK), 'scenarios': []}
    for protocol in ('udp', 'tcp'):
        result = run_case(args.server.resolve(), args.client.resolve(), root, protocol, 'dynacast', 'vp8')
        report['scenarios'].append(result)
        (root / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(root)
    return int(any(not row['ok'] for row in report['scenarios']))


if __name__ == '__main__':
    raise SystemExit(main())
