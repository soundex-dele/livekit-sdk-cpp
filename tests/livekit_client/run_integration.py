"""Run an isolated headless SFU and verify decoded client media (no desktop use)."""

import argparse
import base64
import hashlib
import hmac
import json
import os
from pathlib import Path
import secrets
import socket
import struct
import subprocess
import time


def token(key, secret, identity, room):
    def encode(value):
        return base64.urlsafe_b64encode(json.dumps(value).encode()).rstrip(b"=")
    now = int(time.time())
    payload = encode({"alg": "HS256", "typ": "JWT"}) + b"." + encode({
        "iss": key, "sub": identity, "nbf": now - 30, "exp": now + 600,
        "video": {"roomJoin": True, "room": room, "canPublish": True,
                  "canSubscribe": True, "canPublishData": True},
    })
    signature = base64.urlsafe_b64encode(hmac.new(secret.encode(), payload, hashlib.sha256).digest())
    return (payload + b"." + signature.rstrip(b"=")).decode()


def port(kind):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--client", required=True, type=Path)
    parser.add_argument("--ffmpeg", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--restart", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    signal, tcp, udp = port(socket.SOCK_STREAM), port(socket.SOCK_STREAM), port(socket.SOCK_DGRAM)
    key, secret = "client-test", secrets.token_hex(32)
    config = output / "server.yaml"
    config.write_text(f"""port: {signal}
bind_addresses: [127.0.0.1]
rtc:
  tcp_port: {tcp}
  udp_port: {udp}
  use_external_ip: false
  enable_loopback_candidate: true
keys:
  {key}: {secret}
logging:
  level: warn
""", encoding="utf-8")
    hidden = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    env = dict(os.environ)
    room = "encoded-client-" + secrets.token_hex(4)
    env["LIVEKIT_URL"] = f"ws://127.0.0.1:{signal}"
    env["LIVEKIT_PUBLISH_TOKEN"] = token(key, secret, "publisher", room)
    env["LIVEKIT_SUBSCRIBE_TOKEN"] = token(key, secret, "subscriber", room)
    marker = output / "restart-ready"
    marker.unlink(missing_ok=True)
    if args.restart:
        env["LIVEKIT_CLIENT_RESTART_MARKER"] = str(marker)
    with (output / "server.log").open("w", encoding="utf-8") as log:
        server = subprocess.Popen([str(args.server.resolve()), "--config", str(config)],
                                  stdout=log, stderr=subprocess.STDOUT, creationflags=hidden)
        try:
            deadline = time.monotonic() + 15
            while True:
                if server.poll() is not None:
                    raise RuntimeError("Test server exited; inspect server.log")
                try:
                    with socket.create_connection(("127.0.0.1", signal), timeout=0.3):
                        break
                except OSError:
                    if time.monotonic() > deadline:
                        raise RuntimeError("Test server did not start")
                    time.sleep(0.1)
            with (output / "client.log").open("w", encoding="utf-8") as client_log:
                client = subprocess.Popen([str(args.client.resolve())], cwd=output, env=env,
                                          stdout=client_log, stderr=subprocess.STDOUT,
                                          creationflags=hidden)
                try:
                    if args.restart:
                        deadline = time.monotonic() + 45
                        while not marker.exists():
                            if client.poll() is not None or time.monotonic() > deadline:
                                raise RuntimeError("Client did not reach restart checkpoint")
                            time.sleep(0.1)
                        server.terminate()
                        server.wait(timeout=10)
                        time.sleep(1)
                        server = subprocess.Popen(
                            [str(args.server.resolve()), "--config", str(config)],
                            stdout=log, stderr=subprocess.STDOUT, creationflags=hidden)
                    client.wait(timeout=90)
                finally:
                    if client.poll() is None:
                        client.kill()
                        client.wait()
            if client.returncode:
                raise RuntimeError(f"Client exited {client.returncode}; inspect client.log")
            ivf = output / "livekit-client-received.ivf"
            encoded_frames = struct.unpack_from("<I", ivf.read_bytes(), 24)[0]
            raw = output / "decoded.rgb"
            subprocess.run([str(args.ffmpeg.resolve()), "-v", "error", "-y", "-i", str(ivf),
                            "-vsync", "0", "-pix_fmt", "rgb24", "-f", "rawvideo", str(raw)],
                           check=True, timeout=30, creationflags=hidden)
            pixels = raw.read_bytes()
            frame_size = 64 * 48 * 3
            decoded_frames = len(pixels) // frame_size
            if decoded_frames != encoded_frames or decoded_frames < 10:
                raise RuntimeError("Video decode frame count mismatch")
            colors = set()
            for i in range(decoded_frames):
                r, g, b = pixels[i * frame_size:i * frame_size + 3]
                if r > 180 and b < 60 and g < 60:
                    colors.add("red")
                if b > 180 and r < 60 and g < 60:
                    colors.add("blue")
            if colors != {"red", "blue"}:
                raise RuntimeError(f"Video content mismatch: {colors}")
            result = {"passed": True, "video_encoded_frames": encoded_frames,
                      "video_decoded_frames": decoded_frames, "video_colors": sorted(colors),
                      "audio": "Opus decoded inside integration executable",
                      "server_restart_recovery": args.restart,
                      "device_manual_acceptance": False}
            (output / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
            print(json.dumps(result))
        finally:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
            # Ephemeral test credentials are not needed for retained evidence.
            config.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
