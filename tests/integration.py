"""Runs against a private local LiveKit server. Requires Python 3 and FFmpeg.
All children are owned by this test; logs and recordings are retained in --output.
"""
import argparse
import base64
import hashlib
import hmac
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import time
import wave


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lk", required=True)
    parser.add_argument("--server", required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--output", default="out/integration")
    args = parser.parse_args()
    root = Path(args.output).resolve() / time.strftime("%Y%m%d-%H%M%S")
    root.mkdir(parents=True)
    lk = str(Path(args.lk).resolve())
    port, rtc_port, udp_port = free_port(), free_port(), free_port()
    env = os.environ.copy()
    env.update(LIVEKIT_URL=f"http://127.0.0.1:{port}", LIVEKIT_API_KEY="testkey",
               LIVEKIT_API_SECRET="integration-test-secret-32-characters", LIVEKIT_TOKEN="")
    children = []
    handles = []
    creationflags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0

    def run(*cmd, ok=True, timeout=45):
        result = subprocess.run([lk, *cmd], env=env, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=timeout, creationflags=creationflags)
        if ok and result.returncode:
            raise AssertionError(f"{cmd}: {result.returncode}\n{result.stdout}\n{result.stderr}")
        if not ok and result.returncode == 0:
            raise AssertionError(f"Expected failure: {cmd}")
        return result.stdout

    def start(label, cmd, child_env=env):
        out = open(root / f"{label}.stdout.log", "w", encoding="utf-8")
        err = open(root / f"{label}.stderr.log", "w", encoding="utf-8")
        handles.extend([out, err])
        process = subprocess.Popen(cmd, env=child_env, stdout=out, stderr=err, creationflags=creationflags)
        children.append(process)
        return process

    def events(label):
        output = (root / f"{label}.stdout.log").read_text(encoding="utf-8", errors="replace")
        return [json.loads(line) for line in output.splitlines() if line.startswith("{")]

    def wait_event(label, process, event, timeout=35):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if any(item.get("event") == event for item in events(label)):
                return
            if process.poll() is not None:
                raise AssertionError(f"{label} exited {process.returncode} before {event}; see {root}")
            time.sleep(.1)
        raise AssertionError(f"{label} never emitted {event}; see {root}")

    def check_recording(directory):
        videos = list(directory.glob("*.y4m"))
        audios = list(directory.glob("*.wav"))
        assert videos and audios, f"Missing audio/video recordings in {directory}"
        assert max(p.stat().st_size for p in videos) > 640 * 360 * 3 // 2 * 20
        audible = False
        for path in audios:
            with wave.open(str(path), "rb") as wav:
                assert wav.getsampwidth() == 2
                pcm = wav.readframes(wav.getnframes())
                if pcm:
                    samples = struct.unpack(f"<{len(pcm)//2}h", pcm)
                    audible |= len(samples) > 48000 and max(abs(s) for s in samples) > 100
        assert audible, "WAV must contain decoded non-silent audio, not only a header"

    try:
        config = (f"port: {port}\nbind_addresses: [127.0.0.1]\nrtc:\n"
                  f"  tcp_port: {rtc_port}\n  udp_port: {udp_port}\n  use_external_ip: false\n"
                  "  node_ip: 127.0.0.1\n  enable_loopback_candidate: true\n"
                  f"keys:\n  testkey: {env['LIVEKIT_API_SECRET']}\nlogging:\n  level: info\n")
        (root / "server.yaml").write_text(config, encoding="utf-8")
        server = start("server", [args.server, "--config", str(root / "server.yaml")])
        deadline = time.monotonic() + 20
        while True:
            if server.poll() is not None:
                raise AssertionError(f"Server failed to start; see {root}")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.5):
                    break
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(.1)
        run("doctor")
        token = run("token", "create", "--room", "test", "--identity", "token-test").strip()
        header, claims, signature = token.split(".")
        unb64 = lambda value: base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))
        assert hmac.compare_digest(unb64(signature), hmac.new(env["LIVEKIT_API_SECRET"].encode(), f"{header}.{claims}".encode(), hashlib.sha256).digest())
        assert json.loads(unb64(claims))["video"]["room"] == "test"
        run("room", "create", "test", "--metadata", "initial")
        assert any(room["name"] == "test" for room in json.loads(run("room", "list"))["rooms"])
        run("room", "update", "test", "--metadata", "updated")
        assert json.loads(run("room", "list", "test"))["rooms"][0]["metadata"] == "updated"
        run("--token", "invalid", "room", "join", "test", "--duration", "1", ok=False)
        run("room", "join", "test", "--identity", "invalid-size", "--width", "3", ok=False)
        recording = root / "demo-recording"
        sub = start("demo-sub", [lk, "room", "subscribe", "test", "--identity", "subscriber", "--record", str(recording), "--duration", "12"])
        wait_event("demo-sub", sub, "connected")
        pub = start("demo-pub", [lk, "room", "publish", "test", "--identity", "publisher", "--publish-demo", "--publish-tone", "--publish-data", "hello native", "--topic", "test", "--duration", "6"])
        wait_event("demo-pub", pub, "connected")
        people = json.loads(run("room", "participants", "test"))["participants"]
        assert {p["identity"] for p in people} >= {"publisher", "subscriber"}
        assert pub.wait(timeout=25) == 0
        assert sub.wait(timeout=25) == 0
        check_recording(recording)
        assert any(e.get("event") == "data" for e in events("demo-sub")), "Reliable data not received"
        print("PASS: JWT, room API, native video/Opus, data channel, decoded recording", flush=True)
        fixture = root / "media sample 测试.mkv"
        subprocess.run([args.ffmpeg, "-nostdin", "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25", "-f", "lavfi", "-i", "sine=frequency=660:sample_rate=48000", "-t", "4", "-c:v", "libvpx", "-c:a", "libopus", str(fixture)], check=True, timeout=30, creationflags=creationflags)
        file_recording = root / "file-recording"
        sub = start("file-sub", [lk, "room", "subscribe", "test", "--identity", "file-subscriber", "--record", str(file_recording), "--duration", "10"])
        wait_event("file-sub", sub, "connected")
        # Repeating the flag exercises independent tracks and simultaneous child
        # pipes, and catches CLI parsers that accidentally limit the total to one.
        pub = start("file-pub", [lk, "room", "publish", "test", "--identity", "file-publisher", "--publish", str(fixture), "--publish", str(fixture), "--ffmpeg", args.ffmpeg, "--exit-after-publish"])
        assert pub.wait(timeout=35) == 0, "File publish failed"
        assert sub.wait(timeout=20) == 0
        check_recording(file_recording)
        received = [e for e in events("file-sub") if e.get("event") == "track-summary"]
        assert len(received) == 4 and all(e["frames"] > 20 for e in received), received
        print("PASS: FFmpeg file decode, Unicode paths, repeated publish (4 tracks), EOF shutdown", flush=True)
        run("room", "delete", "test")
        assert not json.loads(run("room", "list", "test")).get("rooms", [])
        (root / "result.json").write_text(json.dumps({"ok": True, "server": args.server, "checks": ["token", "room-api", "negative-inputs", "demo-video", "opus-audio", "data-channel", "file-publish", "unicode-path", "recording", "shutdown"]}, indent=2), encoding="utf-8")
        print(f"PASS: integration; artifacts: {root}", flush=True)
    finally:
        for child in reversed(children):
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=10)
        for handle in handles:
            handle.close()


if __name__ == "__main__":
    main()
