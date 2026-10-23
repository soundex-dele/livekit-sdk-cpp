"""Focused TCP H.264, selective subscription and server disconnect tests."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import time
from integration import free_port


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lk", required=True)
    parser.add_argument("--server", required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    args = parser.parse_args()
    root = Path("out/stream-integration") / time.strftime("%Y%m%d-%H%M%S")
    root.mkdir(parents=True)
    root = root.resolve()
    lk = str(Path(args.lk).resolve())
    port, rtc, udp, source_port = [free_port() for _ in range(4)]
    env = os.environ.copy()
    env.update(LIVEKIT_URL=f"http://127.0.0.1:{port}", LIVEKIT_API_KEY="testkey",
               LIVEKIT_API_SECRET="integration-test-secret-32-characters", LIVEKIT_TOKEN="")
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    children, files = [], []

    def start(label, cmd):
        out = open(root / f"{label}.stdout.log", "w", encoding="utf-8")
        err = open(root / f"{label}.stderr.log", "w", encoding="utf-8")
        files.extend([out, err])
        child = subprocess.Popen(cmd, env=env, stdout=out, stderr=err, creationflags=flags)
        children.append(child)
        return child

    def events(label):
        text = (root / f"{label}.stdout.log").read_text(encoding="utf-8", errors="replace")
        return [json.loads(line) for line in text.splitlines() if line.startswith("{")]

    def wait_event(label, child, name):
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if any(e.get("event") == name for e in events(label)):
                return
            if child.poll() is not None:
                raise AssertionError(f"{label} exited {child.returncode}; see {root}")
            time.sleep(.1)
        raise AssertionError(f"No {name} from {label}; see {root}")

    def run(*cmd):
        result = subprocess.run([lk, *cmd], env=env, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=20, creationflags=flags)
        assert result.returncode == 0, result.stderr
        return json.loads(result.stdout)

    try:
        config = root / "server.yaml"
        config.write_text(f"port: {port}\nbind_addresses: [127.0.0.1]\nrtc:\n  tcp_port: {rtc}\n  udp_port: {udp}\n  use_external_ip: false\n  node_ip: 127.0.0.1\n  enable_loopback_candidate: true\nkeys:\n  testkey: {env['LIVEKIT_API_SECRET']}\nroom:\n  enable_remote_unmute: true\nlogging:\n  level: warn\n", encoding="utf-8")
        server = start("server", [args.server, "--config", str(config)])
        deadline = time.monotonic() + 15
        while True:
            assert server.poll() is None, f"Server failed; see {root}"
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.2):
                    break
            except OSError:
                assert time.monotonic() < deadline
                time.sleep(.1)
        sub = start("all", [lk, "room", "subscribe", "tcp-test", "--identity", "all", "--record", str(root / "all-recording"), "--duration", "14"])
        wait_event("all", sub, "connected")
        producer = start("ffmpeg", [args.ffmpeg, "-nostdin", "-v", "error", "-re", "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30", "-t", "10", "-an", "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency", "-g", "30", "-listen", "1", "-f", "h264", f"tcp://127.0.0.1:{source_port}"])
        time.sleep(1)
        pub = start("publisher", [lk, "room", "publish", "tcp-test", "--identity", "tcp-publisher", "--publish", f"h264://127.0.0.1:{source_port}", "--publish-tone", "--duration", "11"])
        wait_event("publisher", pub, "connected")
        people = run("room", "participants", "tcp-test")["participants"]
        publisher = next(p for p in people if p["identity"] == "tcp-publisher")
        video = next(t["sid"] for t in publisher["tracks"] if t.get("type") == "VIDEO")
        audio = next(t["sid"] for t in publisher["tracks"] if t.get("type") == "AUDIO")
        selected = start("selected", [lk, "room", "subscribe", "tcp-test", "--identity", "selected", "--subscribe-track", video, "--record", str(root / "selected-recording"), "--duration", "7"])
        wait_event("selected", selected, "connected")
        run("room", "mute-track", "tcp-test", "--identity", "tcp-publisher", "--sid", audio)
        time.sleep(1)
        run("room", "mute-track", "tcp-test", "--identity", "tcp-publisher", "--sid", audio, "--unmute")
        assert selected.wait(timeout=20) == 0
        assert pub.wait(timeout=20) == 0
        assert sub.wait(timeout=20) == 0
        assert producer.wait(timeout=10) == 0
        selected_stats = [e for e in events("selected") if e.get("event") == "track-summary"]
        assert selected_stats and all(e["kind"] == "video" for e in selected_stats), selected_stats
        assert selected_stats[0]["frames"] >= 30, selected_stats
        all_stats = [e for e in events("all") if e.get("event") == "track-summary"]
        assert {e["kind"] for e in all_stats} == {"video", "audio"}
        assert any(e["kind"] == "video" and e["frames"] >= 100 for e in all_stats)
        muted = [e["muted"] for e in events("publisher") if e.get("event") == "track-muted"]
        assert True in muted and False in muted, muted
        kicked = start("kicked", [lk, "room", "join", "tcp-test", "--identity", "kicked"])
        wait_event("kicked", kicked, "connected")
        run("room", "remove-participant", "tcp-test", "--identity", "kicked")
        assert kicked.wait(timeout=15) != 0
        run("room", "delete", "tcp-test")
        summary = {"ok": True, "selected": selected_stats, "all": all_stats,
                   "checks": ["tcp-h264", "selective-subscription", "mute-unmute", "server-disconnect"]}
        (root / "result.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
        print(f"PASS: TCP H.264, selective subscription, mute/unmute, disconnect; artifacts: {root}")
    finally:
        for child in reversed(children):
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=10)
        for f in files:
            f.close()


if __name__ == "__main__":
    main()
