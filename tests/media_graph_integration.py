"""Real SDK -> C++ SFU -> SDK graph regression. Owns an isolated local server.

python tests/media_graph_integration.py --lk out/windows-x64/Release/lk.exe \
    --server /path/to/livekit-server.exe [--sdk-client /path/to/livekit_sdk_integration.exe]
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import time
import urllib.request
import wave


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lk", required=True)
    parser.add_argument("--server", required=True)
    parser.add_argument("--sdk-client")
    parser.add_argument("--demo-client", help="Headless conference session integration (no application window)")
    parser.add_argument("--output", default="out/media-graph-integration")
    args = parser.parse_args()
    root = Path(args.output).resolve() / time.strftime("%Y%m%d-%H%M%S")
    root.mkdir(parents=True)
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    env = dict(os.environ, LIVEKIT_URL=f"http://127.0.0.1:{port}", LIVEKIT_API_KEY="graphtest",
               LIVEKIT_API_SECRET="isolated-graph-test-secret-32-characters", LIVEKIT_TOKEN="", LIVEKIT_DEMO_TEST_AUDIO="0")
    config = root / "server.json"
    config.write_text(json.dumps({"bind": "127.0.0.1", "port": port,
        "keys": {"graphtest": env["LIVEKIT_API_SECRET"]},
        "rtc": {"port_range_start": 51200, "port_range_end": 51300}}))
    children, handles = [], []
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0

    def start(label, command):
        out = (root / f"{label}.stdout.log").open("w", encoding="utf-8")
        err = (root / f"{label}.stderr.log").open("w", encoding="utf-8")
        handles.extend([out, err])
        child = subprocess.Popen(command, env=env, stdout=out, stderr=err, creationflags=flags)
        children.append(child)
        return child

    def events(label):
        return [json.loads(line) for line in (root / f"{label}.stdout.log").read_text(encoding="utf-8").splitlines()
                if line.startswith("{")]

    def wait_connected(label, child):
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if any(event.get("event") == "connected" for event in events(label)):
                return
            assert child.poll() is None, f"{label} exited early; see {root}"
            time.sleep(.1)
        raise AssertionError(f"{label} connect timed out; see {root}")

    def require_node(label, kind):
        nodes = [node for event in events(label) if event.get("event") == "media-graph-stats"
                 for node in event["nodes"] if node["kind"] == kind]
        assert any(node["inputs"] > 0 and node["outputs"] > 0 for node in nodes), f"{label}: inactive {kind}"
        assert all(node["errors"] == 0 for node in nodes), f"{label}: errors in {kind}"

    def require_edge(label, source, target, data_type):
        for event in events(label):
            if event.get("event") != "media-graph-stats":
                continue
            kinds = {node["name"]: node["kind"] for node in event["nodes"]}
            if any(kinds.get(edge["from"]) == source and kinds.get(edge["to"]) == target
                   and edge["dataType"] == data_type for edge in event.get("edges", [])):
                return
        raise AssertionError(f"{label}: missing GraphEngine edge {source} -> {target} ({data_type})")

    try:
        server = start("server", [str(Path(args.server).resolve()), "--config", str(config)])
        deadline = time.monotonic() + 20
        while True:
            assert server.poll() is None, f"Server exited; see {root}"
            try:
                with urllib.request.urlopen(env["LIVEKIT_URL"] + "/healthz", timeout=1):
                    break
            except OSError:
                assert time.monotonic() < deadline, "Server startup timed out"
                time.sleep(.1)
        lk = str(Path(args.lk).resolve())
        recordings = root / "recordings"
        subscriber = start("subscriber", [lk, "room", "subscribe", "graph-regression", "--identity", "receiver",
            "--duration", "10", "--record", str(recordings), "--stats-interval-ms", "500"])
        wait_connected("subscriber", subscriber)
        publisher = start("publisher", [lk, "room", "publish", "graph-regression", "--identity", "sender",
            "--duration", "6", "--publish-demo", "--publish-tone", "--publish-data", "graph-test", "--stats-interval-ms", "500"])
        wait_connected("publisher", publisher)
        assert publisher.wait(timeout=30) == 0 and subscriber.wait(timeout=30) == 0, f"RTC client failure; see {root}"
        for kind in ("VideoSourceNode", "AudioSourceNode", "RtcVideoTrackSinkNode",
                     "RtcPublishNode", "VideoEncoderNode", "AudioEncoderNode"):
            require_node("publisher", kind)
        for kind in ("VideoDecoderNode", "AudioDecoderNode", "RtcSubscribeNode", "VideoRenderNode", "AudioOutputNode", "RecordingNode"):
            require_node("subscriber", kind)
        require_edge("publisher", "VideoSourceNode", "VideoEncoderNode", "RawVideoFrame")
        require_edge("publisher", "VideoEncoderNode", "RtcVideoTrackSinkNode", "EncodedVideoFrame")
        assert not any(node["kind"] == "VideoEncodeInputNode" for event in events("publisher")
                       if event.get("event") == "media-graph-stats" for node in event["nodes"])
        require_edge("subscriber", "RtcEncodedSourceNode", "VideoDecoderNode", "VideoDecodeRequest")
        require_edge("subscriber", "VideoDecoderNode", "VideoPresentNode", "RawVideoFrame")
        require_edge("subscriber", "VideoPresentNode", "VideoRenderNode", "RawVideoFrame")
        require_edge("subscriber", "VideoDecoderNode", "RecordingNode", "RawVideoFrame")
        assert not any(node["kind"] == "RtcDecodeOutputNode" for event in events("subscriber")
                       if event.get("event") == "media-graph-stats" for node in event["nodes"])
        assert any(event.get("event") == "data" for event in events("subscriber")), "Data channel regression"
        videos = list(recordings.glob("*.y4m"))
        assert videos and max(path.stat().st_size for path in videos) > 640 * 360 * 3 // 2 * 20, "Missing decoded video"
        audible = False
        for path in recordings.glob("*.wav"):
            with wave.open(str(path)) as wav:
                samples = wav.readframes(wav.getnframes())
                pcm = struct.unpack(f"<{len(samples) // 2}h", samples)
                audible |= len(pcm) > 48000 and max(map(abs, pcm)) > 100
        assert audible, "Missing decoded audio"
        if args.sdk_client:
            client = start("sdk-api", [str(Path(args.sdk_client).resolve()), str(root / "sdk-recordings")])
            assert client.wait(timeout=110) == 0, f"SDK dynamic publish/subscription regression; see {root}"
        if args.demo_client:
            client = start("conference", [str(Path(args.demo_client).resolve()), str(root / "conference-recordings")])
            assert client.wait(timeout=180) == 0, f"Headless conference frame routing regression; see {root}"
        report = {"ok": True, "factories": 4, "graphDrivenVideoEncoding": True, "graphEngineTopology": True,
                  "directGraphVideoReceive": True,
                  "headlessConferenceRegression": bool(args.demo_client),
                  "actualMedia": True, "recordings": True,
                  "sdkApiRegression": bool(args.sdk_client), "server": str(Path(args.server).resolve())}
        report["serverSha256"] = hashlib.sha256(Path(args.server).read_bytes()).hexdigest()
        report["cliSha256"] = hashlib.sha256(Path(args.lk).read_bytes()).hexdigest()
        (root / "result.json").write_text(json.dumps(report, indent=2))
        print(json.dumps(report), root)
    finally:
        for child in reversed(children):
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)
        for handle in handles:
            handle.close()


if __name__ == "__main__":
    main()
