"""Go SFU + native C++ SDK evidence runner (stdlib only, no desktop/devices).

Builds the specified Go checkout, never substitutes a historical server binary.
Returns 1 for failed probes/known requirement gaps, 2 for setup failure, 0 only
when executed checks pass without a detected gap. Even 0 is NOT full acceptance:
the report retains untested requirements, device tests and capacity separately.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone, timedelta
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import time
from simulation_acceptance import simulation_rows

SDK = Path(__file__).resolve().parents[1]
WORKSPACE = SDK.parent
FLAGS = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
REQUIREMENTS = {
    "V-01": ("多摄像头识别", "video", "实机热插拔、枚举变化、deviceId/cameraId 跨重连稳定映射"),
    "V-02": ("多用户独立观看", "video", "H.265、目标分辨率及混合负载"),
    "V-08": ("聚合订阅控制源", "go", "真实机载编码器收到 OFF 后停止上传，恢复后重启"),
    "V-11": ("Web 多机观看", "", "Web 多 Room、多机授权与实际并发解码"),
    "V-12": ("H.265→H.264 全体回退", "h265", "首位能力选择、机载切换、所有观看者解码及重连"),
    "V-13": ("实时两段 UDP", "video", "目标板与三端；与回放同时运行时的隔离及重连"),
    "A-01": ("多人全双工", "audio", "扬声器混音、完整播放参考 AEC、实测每条有向链路时延"),
    "A-02": ("独立成员管理", "audio", "授权、人数限制、异常成员隔离与容量提示"),
    "B-01": ("回放源接入", "tcp", "真实文件读取、原样编码、时间戳和源节奏"),
    "B-03": ("多回放会话", "tcp", "独立 seek/倍速、playbackSessionId、与实时语音并行"),
    "B-04": ("同回放源多人分发", "tcp", "TCP 慢订阅者、写入超时、共享源隔离"),
    "B-07": ("回放两段 TCP", "tcp", "目标端主动 ICE-TCP、真实回放与实时独立连接、重连"),
    "N-02": ("弱网音频和主画面优先", "", "分别限制上/下行带宽，验证主辅画面和音频预算"),
    "Q-01": ("BWE/拥塞控制", "", "受控弱网及目标编码器目标/实际码率闭环"),
    "Q-02": ("NACK/RTX 有界重传", "", "独立上下行丢包注入、重传预算配置与过期淘汰"),
    "Q-03": ("PLI/关键帧恢复", "", "多人同源丢关键帧、PLI 限频与 H.265/H.264 解码恢复"),
    "Q-05": ("独立订阅质量", "video", "弱网预算、层选择/暂停、TCP 慢端不影响其他观看者"),
    "Q-06": ("pacing/有界队列", "go", "真正码率节奏、有界队列/TTL、完整帧丢弃和 TCP 截止时间"),
    "Q-07": ("音频和视频 FEC", "go", "视频 FEC 协商/上行恢复/下行保护，以及端侧 PLC/FEC 丢包测试"),
    "Q-08": ("1 s 全链路统计", "go", "设备/方向关联、1 s 采样、冻结/中断、重连增量"),
    "N-01": ("P95 时延", "", "同步采集/呈现时间，实时/回放 ≤500 ms，六条音频链路 ≤300 ms"),
    "N-06": ("首帧/加入语音", "", "重复冷热加入样本，首帧 P95 ≤2 s、双向可听 ≤3 s"),
    "P-02": ("接入容量", "", "固定扇出递增 H.265 720p30/2 Mbps 源，固定 Opus 32 kbps 负载"),
    "P-03": ("转发容量", "", "固定源数递增订阅；UDP/TCP/混合、回退峰值、音频扇出"),
    "P-10": ("候选最高负载 8 h", "", "连续 8 h 资源/质量记录，未触边界仅报告下限"),
    "P-11": ("配额/过载保护", "", "动态并发发布/订阅，源/连接/转发/缓存资源预留与释放"),
    "V-04": ("单摄像头故障隔离", "video", "真实拔插/硬编异常/无帧超时，不触发全设备重连"),
    "A-05": ("成员退出/清理", "audio", "授权过期主动撤权与异常断连清理；正常音频撤轨已有子项"),
    "A-06": ("幂等/代次隔离", "", "重放 requestId、旧 connectionEpoch 的迟到移除不影响新成员"),
    "B-06": ("回放结束/重发", "tcp", "真实 EOS/故障原因、稳定 streamId、新代次及旧流隔离"),
    "N-04": ("网络变化恢复", "", "网卡变化/断网重连，实时仍 UDP、回放仍 TCP、语音重新授权"),
    "N-05": ("节点故障重建", "", "终止独立测试节点，检查重新授权/媒体重建/幽灵成员清理"),
    "N-08": ("机载资源/长时运行", "", "目标 ARM64 板/手机 8 h，硬编/温度/功耗/资源释放"),
    "N-09": ("可观测追溯", "go", "业务 ID 关联、终端质量与故障看板"),
}

# Source assessment, separate from executed measurements. Review these entries
# when the corresponding implementation is delivered; successful static-codec
# smoke tests cannot clear an absent regression/FEC/business pipeline.
KNOWN_IMPLEMENTATION_GAPS = {
    "V-12": "SDK 已提供房间级统一回退及独立 rtc_codec_policy 集成入口；本运行器的独立 H.264/H.265 场景不能证明全体回退或机载设备切换。",
    "Q-06": "当前发送路径缺少与 BWE 联动的 pacing、队列字节/时长硬边界及完整帧依赖恢复。",
    "Q-07": "视频 FEC 协商、生成及恢复尚无端到端实现；Opus FEC 或 RTX 不能替代。",
    "Q-08": "1 s 基础统计已接通，但业务标识、冻结/音频中断及重连增量基线仍未闭环。",
    "P-11": "尚缺源/转发/连接/总缓存的严格动态原子准入；Windows CPU 容量监控不可用。",
    "N-09": "尚缺 deviceId/cameraId/playbackSessionId/epoch 全链路关联及终端质量看板。",
}


def command(argv, cwd, log, env=None, timeout=600):
    with log.open("w", encoding="utf-8") as stream:
        try:
            return subprocess.run(list(map(str, argv)), cwd=cwd, env=env,
                                  stdout=stream, stderr=subprocess.STDOUT,
                                  creationflags=FLAGS, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            stream.write("\nHARNESS_TIMEOUT\n")
            return 124


def git_info(root):
    def read(*args):
        return subprocess.check_output(["git", "-C", str(root), *args], text=True, encoding="utf-8").strip()
    return {"path": str(root), "commit": read("rev-parse", "HEAD"), "status": read("status", "--short")}


def digest(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def protocol_evidence(log, protocol, scenario):
    pairs = []
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            # JSON logging is configured below; tolerate a textual log prefix.
            row = json.loads(line[line.index("{"):])
        except (ValueError, json.JSONDecodeError):
            continue
        if "selected ICE candidate pair changed" not in line:
            continue
        pair = row.get("pair", {})
        pairs.append({"participant": row.get("participant"), "transport": row.get("transport"), **pair})
    if scenario == "transport-reject":
        return {"id": "no_selected_ice_pair", "status": "PASS" if not pairs else "FAIL", "pairs": pairs}
    required = ({("drone", "PUBLISHER"), ("viewer-a", "SUBSCRIBER"), ("viewer-b", "SUBSCRIBER")}
                if scenario in ("video", "dynacast") else
                {(p, t) for p in ("drone", "viewer-a", "viewer-b") for t in ("PUBLISHER", "SUBSCRIBER")})
    def direct_pair(p):
        return (p.get("localProtocol") == protocol and p.get("remoteProtocol") == protocol
                and p.get("localCandidateType") in ("host", "srflx", "prflx")
                and p.get("remoteCandidateType") in ("host", "srflx", "prflx"))
    observed = {(p["participant"], p["transport"]) for p in pairs if direct_pair(p)}
    invalid = [p for p in pairs if not direct_pair(p)]
    return {"id": "selected_ice_protocol", "status": "PASS" if required <= observed and not invalid else "FAIL",
            "expected": protocol, "missing": sorted(required - observed), "pairs": pairs}


def run_case(server, client, root, protocol, scenario, codec):
    folder = root / f"{protocol}-{scenario}-{codec}"
    folder.mkdir()
    port, media = free_port(), free_port(socket.SOCK_DGRAM if protocol == "udp" else socket.SOCK_STREAM)
    while media == port:
        media = free_port(socket.SOCK_DGRAM if protocol == "udp" else socket.SOCK_STREAM)
    config = folder / "server.yaml"
    config.write_text(
        f"port: {port}\nbind_addresses: [127.0.0.1]\nrtc:\n"
        f"  tcp_port: {media if protocol == 'tcp' else 0}\n"
        f"  udp_port: {media if protocol == 'udp' else 0}\n"
        f"  force_tcp: {'true' if protocol == 'tcp' else 'false'}\n"
        "  allow_tcp_fallback: false\n  allow_udp_unstable_fallback: false\n"
        "  turn_servers: []\n  use_external_ip: false\n  node_ip: 127.0.0.1\n"
        "  ips:\n    includes: [127.0.0.1/32]\n"
        "  enable_loopback_candidate: true\nturn:\n  enabled: false\nkeys:\n"
        "  uav-test: uav-local-test-secret-at-least-32-characters\n"
        "logging:\n  level: debug\n  json: true\n", encoding="utf-8")
    result = {"scenario": scenario, "protocol": protocol, "codec": codec, "folder": folder.name,
              "ok": False, "checks": [], "started_at": datetime.now(timezone.utc).isoformat()}
    with (folder / "server.log").open("w", encoding="utf-8") as log:
        process = subprocess.Popen([str(server), "--config", str(config)], stdout=log, stderr=subprocess.STDOUT, creationflags=FLAGS)
        try:
            deadline = time.monotonic() + 25
            while True:
                if process.poll() is not None:
                    raise RuntimeError("server exited during startup")
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=.3):
                        break
                except OSError:
                    if time.monotonic() > deadline:
                        raise RuntimeError("server readiness timeout")
                    time.sleep(.1)
            client_transport = ("tcp" if protocol == "udp" else "udp") if scenario == "transport-reject" else protocol
            rc = command([client, f"ws://127.0.0.1:{port}", scenario, codec, folder, client_transport], SDK, folder / "client.log", timeout=130)
            if (folder / "client-result.json").exists():
                result.update(json.loads((folder / "client-result.json").read_text(encoding="utf-8")))
            result["exit_code"] = rc
            result["ok"] = rc == 0 and result["ok"]
            if rc and "error" not in result:
                result["error"] = "client timeout or crash; see client.log"
        except Exception as e:
            result["setup_error"] = str(e)
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait(timeout=5)
    # H265 validation can fail before opening a connection; do not fabricate a protocol failure.
    if result["checks"]:
        protocol_check = protocol_evidence(folder / "server.log", protocol, scenario)
        result["checks"].append(protocol_check)
        result["ok"] = result["ok"] and protocol_check["status"] == "PASS"
    result["finished_at"] = datetime.now(timezone.utc).isoformat()
    (folder / "result.json").write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"{folder.name}: {'PASS' if result['ok'] else 'FAIL'}", flush=True)
    return result


def go_probes(args, root, env):
    replacements = {str(args.server_source / f"pkg/{package}/uav_requirements_overlay_test.go"):
                    str(SDK / "tests/go_requirements" / name)
                    for package, name in [("config", "config_test.go"), ("sfu/pacer", "pacer_test.go"),
                                          ("rtc/dynacast", "dynacast_test.go")]}
    overlay = root / "overlay.json"
    overlay.write_text(json.dumps({"Replace": replacements}), encoding="utf-8")
    rc = command([args.go, "test", "-mod=readonly", "-overlay", overlay, "-json", "-count=1", "-timeout=120s",
                  "-run", "TestUAV", "./pkg/config", "./pkg/sfu/pacer", "./pkg/rtc/dynacast"],
                 args.server_source, root / "go-probes.jsonl", env)
    evidence, tests = [], []
    for line in (root / "go-probes.jsonl").read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            row = json.loads(line)
            if row.get("Action") in ("pass", "fail") and "Test" in row:
                tests.append({k: row[k] for k in ("Package", "Test", "Action")})
            if row.get("Output", "").startswith("UAV_EVIDENCE "):
                evidence.append(json.loads(row["Output"][len("UAV_EVIDENCE "):]))
        except json.JSONDecodeError:
            continue
    return {"exit_code": rc, "tests": tests, "evidence": evidence}


def finish(report, root):
    scenarios = report.get("scenarios", [])
    groups = {"video": [r for r in scenarios if r["protocol"] == "udp" and r["scenario"] == "video" and r["codec"] == "h264"],
              "tcp": [r for r in scenarios if r["protocol"] == "tcp" and r["scenario"] != "transport-reject"],
              "audio": [r for r in scenarios if r["scenario"] in ("audio", "mixed")],
              "h265": [r for r in scenarios if r["codec"] == "h265"],
              "dynacast": [r for r in scenarios if r["scenario"] == "dynacast"]}
    # Do not attribute implementation failures when setup never reached a build.
    assessed = dict(KNOWN_IMPLEMENTATION_GAPS) if report.get("sha256", {}).get("server") else {}
    report["source_assessment_gaps"] = assessed
    gaps = dict(assessed)
    # A failed executed child assertion must not disappear into "untested".
    for r in scenarios:
        if not r.get("ok") and not r.get("setup_error"):
            policy = r["protocol"]
            if r["scenario"] == "transport-reject":
                policy = "tcp" if policy == "udp" else "udp"
            affected = (["V-13"] if policy == "udp" else ["B-07"])
            if r["scenario"] in ("audio", "mixed"):
                affected += ["A-01", "A-02", "A-05"]
            elif r["scenario"] == "video":
                affected += (["V-01", "V-02", "V-04", "Q-05"] if r["protocol"] == "udp" else ["B-01", "B-03", "B-04", "B-06"])
            for rid in affected:
                gaps[rid] = f"关联自动化场景 {r['folder']} 未通过（不表示本行所有子项均失败）：" + str(r.get("error", "见场景日志"))
    for r in groups["tcp"]:
        if not r.get("ok"):
            gaps["B-07"] = "本次 TCP 媒体子项失败：" + str(r.get("error", r.get("setup_error", "见场景日志")))
    for r in groups["dynacast"]:
        if not r.get("ok"):
            gaps["V-08"] = "发布端聚合需求控制失败：" + str(r.get("error", r.get("setup_error", "见场景日志")))
    for r in groups["h265"]:
        if r.get("error") == "Unsupported video codec":
            gaps["V-12"] = "C++ SDK 在连接前拒绝 H.265：Unsupported video codec。无法完成全体回退。"
    for e in report.get("go", {}).get("evidence", []):
        if e["id"] == "noqueue_growth" and e["queued_packets"] > e["initial_capacity"]:
            gaps["Q-06"] = f"阻塞发送时队列保留 {e['queued_packets']} 包/{e['queued_payload_bytes']} 字节；512 是初始容量，非硬上限。"
        if e["id"] == "defaults":
            if not e["video_fec_registered"]:
                gaps["Q-07"] = "锁定版本默认视频 codec 集合没有 FEC；未做实际丢包恢复验收。"
            if e["node_stats_interval_ms"] > 1000:
                gaps["Q-08"] = f"默认节点采样 {e['node_stats_interval_ms']} ms，不满足默认全链路 1 s。"
    rows = []
    for rid, (title, group, remaining) in REQUIREMENTS.items():
        refs = [r["folder"] for r in groups.get(group, [])]
        if group == "go" and "go" in report:
            refs = ["go-probes.jsonl"]
        if rid == "V-08":
            refs += [r["folder"] for r in groups["dynacast"]]
        if rid in ("V-13", "B-07"):
            refs += [r["folder"] for r in scenarios if r["scenario"] == "transport-reject"
                     and r["protocol"] == ("udp" if rid == "B-07" else "tcp")]
        if rid in ("V-01", "V-02", "V-13", "A-01", "A-02", "A-05"):
            refs += [r["folder"] for r in scenarios if r["scenario"] == "mixed" and r["folder"] not in refs]
        rows.append({"id": rid, "title": title, "status": "未通过" if rid in gaps else "未测",
                     "evidence": refs, "reason": gaps.get(rid, "仅覆盖子项或尚无自动化证据；完整需求未验收"), "remaining": remaining})
    report["product_requirements"] = rows
    gaps.update(report.get("open_issues", {}))
    rows = simulation_rows(rows, scenarios, report.get("recovery", []), gaps,
                           report.get("component_evidence", False))
    report["requirements"] = rows
    report["acceptance_scope"] = "simulation"
    report["simulation_acceptance"] = (bool(rows) and not report.get("setup_error") and
        report.get("go", {}).get("exit_code") == 0 and all(r["status"] in ("通过", "不适用") for r in rows))
    report["full_acceptance"] = False
    report["device_acceptance"] = "不适用：用户要求模拟验收，不要求真机"
    report["gaps"] = gaps
    (root / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    lines = ["# Go SFU + C++ SDK 自动化需求验证", "", f"运行时间：{report['started_at']}", "",
             "按用户要求以模拟测试验收，真机不作为门槛。缺失的软件行为、弱网、P95、容量及 8 h 模拟证据仍需补齐；保留实际解码、协议及失败断言。", "",
             "## 执行结果", "", "| 场景 | 结果 | 证据 |", "| --- | --- | --- |"]
    for r in scenarios:
        source = Path(report.get("evidence_reuse", {}).get("media_report", str(root / "result.json"))).parent
        target = Path(os.path.relpath(source / r['folder'] / 'result.json', root)).as_posix()
        lines.append(f"| {r['folder']} | {'通过' if r['ok'] else '未通过'} | [{r['folder']}/result.json]({target}) |")
    lines += ["", "## 34 项模拟验收", "", "| 编号 | 需求 | 模拟状态 | 覆盖情况 | 子项证据 | 结论或待补模拟 |", "| --- | --- | --- | --- | --- | --- |"]
    for r in rows:
        refs = ", ".join(r["evidence"]) or "无"
        lines.append(f"| {r['id']} | {r['title']} | {r['status']} | {r['coverage']} | {refs} | {r['reason']} |")
    if "setup_error" in report:
        lines += ["", "环境阻塞：" + report["setup_error"]]
    lines += ["", "版本、工作区修改状态、二进制 SHA256、Go 测试结果见 [result.json](result.json)。", ""]
    (root / "report.md").write_text("\n".join(lines), encoding="utf-8")
    print(root / "report.md", flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--server-source", type=Path, default=WORKSPACE.parent / "livekit")
    p.add_argument("--go", type=Path, default=WORKSPACE / "out/tools/go/bin/go.exe")
    p.add_argument("--client", type=Path, default=SDK / "out/windows-x64/Release/livekit_uav_requirements.exe")
    p.add_argument("--goproxy", help="Process-local module proxy; go.mod/go.sum stay readonly")
    p.add_argument("--output", type=Path)
    p.add_argument("--tcp-repeats", type=int, default=1, help="Total isolated TCP video rounds per codec (1..20)")
    args = p.parse_args()
    if not 1 <= args.tcp_repeats <= 20:
        p.error("--tcp-repeats must be 1..20")
    args.server_source, args.go, args.client = (x.resolve() for x in (args.server_source, args.go, args.client))
    now = datetime.now(timezone(timedelta(hours=8)))
    root = (args.output or WORKSPACE / "out/uav-sfu-go" / now.strftime("%Y%m%d-%H%M%S-%f")).resolve()
    root.mkdir(parents=True, exist_ok=False)
    report = {"started_at": now.isoformat(), "scenarios": []}
    env = dict(os.environ)
    if args.goproxy:
        env["GOPROXY"] = args.goproxy
    try:
        report["server_source"] = git_info(args.server_source)
        report["cpp_source"] = git_info(SDK)
        report["webrtc_source"] = git_info(WORKSPACE / "webrtc")
        report["go_version"] = subprocess.check_output([str(args.go), "version"], text=True).strip()
        if not args.client.is_file():
            raise RuntimeError("Build livekit_uav_requirements CMake target first")
        server = root / ("livekit-server.exe" if os.name == "nt" else "livekit-server")
        rc = command([args.go, "build", "-mod=readonly", "-o", server, "./cmd/server"], args.server_source, root / "go-build.log", env)
        if rc:
            raise RuntimeError(f"Go source build failed ({rc}); see go-build.log")
        report["sha256"] = {"server": digest(server), "cpp_client": digest(args.client),
                            "go_mod": digest(args.server_source / "go.mod"), "go_sum": digest(args.server_source / "go.sum")}
        report["cpp_client"] = str(args.client)
        command([args.go, "version", "-m", server], args.server_source, root / "server-build-info.txt", env)
        report["go"] = go_probes(args, root, env)
        for protocol, scenario, codec in [("udp", "video", "h264"), ("tcp", "video", "h264"),
                                          ("udp", "audio", "h264"), ("udp", "video", "h265"),
                                          ("udp", "dynacast", "h264"), ("udp", "dynacast", "h265"),
                                          ("tcp", "video", "h265"),
                                          ("udp", "transport-reject", "h264"), ("tcp", "transport-reject", "h264")]:
            report["scenarios"].append(run_case(server, args.client, root, protocol, scenario, codec))
        mixed_root = root / "simultaneous"
        mixed_root.mkdir()
        with ThreadPoolExecutor(max_workers=2) as pool:
            jobs = [pool.submit(run_case, server, args.client, mixed_root, protocol, scenario, "h265")
                    for protocol, scenario in [("udp", "mixed"), ("tcp", "video")]]
            for job in jobs:
                result = job.result()
                result["folder"] = "simultaneous/" + result["folder"]
                report["scenarios"].append(result)
        for repeat in range(2, args.tcp_repeats + 1):
            repeat_root = root / f"tcp-repeat-{repeat}"
            repeat_root.mkdir()
            for codec in ("h264", "h265"):
                result = run_case(server, args.client, repeat_root, "tcp", "video", codec)
                result["folder"] = repeat_root.name + "/" + result["folder"]
                report["scenarios"].append(result)
        finish(report, root)
        return 1 if report["gaps"] or report["go"]["exit_code"] or any(not r["ok"] for r in report["scenarios"]) else 0
    except Exception as e:
        report["setup_error"] = str(e)
        finish(report, root)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
