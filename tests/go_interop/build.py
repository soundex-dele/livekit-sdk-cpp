"""Build the optional Go interop fixture in out/, leaving the reference checkout untouched."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sdk', required=True, type=Path)
    parser.add_argument('--go', required=True, type=Path)
    parser.add_argument('--compiler-bin', type=Path, help='Optional MinGW bin directory containing gcc/pkg-config')
    parser.add_argument('--proxy', help='Process-local GOPROXY, checksum verification remains enabled')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    stage = root / 'out/go-interop-peer'
    stage.mkdir(parents=True, exist_ok=True)
    sdk = args.sdk.resolve()
    shutil.copy2(Path(__file__).with_name('main.go'), stage / 'main.go')
    shutil.copy2(sdk / 'go.sum', stage / 'go.sum')
    (stage / 'go.mod').write_text('module livekit-cpp-interop\n\ngo 1.26.8\n\nrequire github.com/livekit/server-sdk-go/v2 v2.19.0\n\nreplace github.com/livekit/server-sdk-go/v2 => ' + json.dumps(sdk.as_posix()) + '\n')
    env = dict(os.environ, CGO_ENABLED='1', GOTOOLCHAIN='local', GOMODCACHE=str(root / 'out/go-mod-cache'), GOCACHE=str(root / 'out/go-build-cache'))
    if args.compiler_bin:
        compiler = args.compiler_bin.resolve()
        env['PATH'] = str(compiler) + os.pathsep + env['PATH']
        env['CC'] = str(compiler / ('gcc.exe' if os.name == 'nt' else 'gcc'))
    if args.proxy:
        env['GOPROXY'] = args.proxy
    binary = stage / ('go-interop.exe' if os.name == 'nt' else 'go-interop')
    subprocess.run([str(args.go.resolve()), '-C', str(stage), 'build', '-mod=mod', '-tags', 'nolibopusfile', '-o', str(binary), '.'], env=env, check=True)
    audit = {'sdk_revision': subprocess.check_output(['git', '-C', str(sdk), 'rev-parse', 'HEAD'], text=True).strip(),
             'sdk_status': subprocess.check_output(['git', '-C', str(sdk), 'status', '--porcelain'], text=True),
             'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
             'go_version': subprocess.check_output([str(args.go.resolve()), 'version'], env=env, text=True).strip()}
    (stage / 'build.json').write_text(json.dumps(audit, indent=2))
    print(binary)


if __name__ == '__main__':
    main()
