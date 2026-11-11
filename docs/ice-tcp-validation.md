# 同一 Go SFU 实例的直接 ICE UDP/TCP 验证

日期：2026-11-09。Windows x64，本机 loopback，无窗口、无物理设备。

## 结论

同一个 Go LiveKit 进程可以同时接受直接 ICE-UDP 和 ICE-TCP。SDK 客户端设置
`ConnectionOptions::media_transport = MediaTransport::tcp_only` 后，可以仅使用
TCP 媒体连接；无需按客户端切换服务端配置或重启实例。UDP 发布者与 TCP 订阅者、
TCP 发布者与 UDP 订阅者也可以在同一房间传输媒体。

现有 SDK 在 `src/backends/livekit/peer_transport.cpp` 中启用 TCP 候选，并设置
`PORTALLOCATOR_DISABLE_UDP | PORTALLOCATOR_DISABLE_STUN | PORTALLOCATOR_DISABLE_RELAY`。
这是禁用本地 UDP 候选收集，不是修改服务端全局候选，也不是 TURN/TCP。
本次没有修改该后端实现或 Go 源码，仅让四个音视频示例通过
`LIVEKIT_MEDIA_TRANSPORT=automatic|udp|tcp` 使用已有选项。

## 环境与方法

- Go 源码：`D:/workspace/livekit`，HEAD `38a3fb7954bedab85dee5b478f61b178611235c9`。
  使用该目录已有本地修改，未回退或改动这些修改；因此不代表纯上游提交测试。
- Go 工具链：`D:/workspace/av-platform/out/tools/go/bin/go.exe`，go1.27.3 windows/amd64。
- 服务端构建：在 Go 源码目录运行 `go build -o <output>/livekit-server.exe ./cmd/server`。
- SDK：构建当前源码的四个 Release 示例，使用 MediaEngine / GraphEngine，合成 VP8 视频、
  生成 PCM 经 Opus 编码，接收端分别实际解码为像素和 PCM。
- 同一服务进程 PID 7172，信令 TCP 58708，ICE TCP 58709，ICE UDP 59200。
- 同一种媒体的四组测试并行运行在四个房间，各房间一发一收；先视频后音频，中间没有重启服务。
- 服务端开启 debug JSON 日志，解析 `selected ICE candidate pair changed`，要求每个客户端
  的所有已选候选对两端协议符合配置，并核对服务端端口；额外要求发布端存在 PUBLISHER
  候选对、订阅端存在 SUBSCRIBER 候选对。不能用 WebSocket 建连代替媒体连接证明。
- 客户端全部退出码为 0，测试结束后关闭本次创建的进程并移除含临时密钥的配置文件。

复现时 Go 服务配置同时包含以下项目，端口由脚本动态分配：

```yaml
bind_addresses: [127.0.0.1]
rtc:
  tcp_port: 58709
  udp_port: 59200
  use_external_ip: false
  enable_loopback_candidate: true
  node_ip: 127.0.0.1
  ips:
    includes: [127.0.0.1/32]
```

## 结果

方向表示发布客户端的 ICE 协议 → 订阅客户端的 ICE 协议。

| 方向 | VP8 解码帧数 | Opus 解码采样/声道 | 实际 ICE 协议及端口检查 |
| --- | ---: | ---: | --- |
| UDP → UDP | 50 | 48480 | 通过 |
| TCP → TCP | 50 | 48000 | 通过 |
| UDP → TCP | 50 | 48480 | 通过 |
| TCP → UDP | 50 | 48000 | 通过 |

8/8 通过。TCP 候选对服务端为 host、客户端为 prflx，协议两端均为 tcp，
服务端端口统一为 58709；prflx 为 ICE 候选类型，不表示使用 TURN。

音频 UDP → TCP 的应用消费队列记录丢弃 1 个块，其余三组为 0；本测试验证连通性和
实际解码，不据此声明无丢帧、音频连续性、弱网性能或长时间稳定性。
实际设备、跨机、防火墙、NAT 和公网网络均未验收。

## 复现与证据

从 SDK 目录运行，输出目录必须尚不存在：

```powershell
cmake --build out/windows-x64 --config Release --target livekit_video_source_example livekit_video_stream_example livekit_audio_source_example livekit_audio_stream_example --parallel 3
python tests/ice_transport_examples_integration.py --server ../out/ice-tcp-validation/livekit-server.exe --bin out/windows-x64/Release --output out/ice-transport-validation-new
```

本次证据位于 `out/ice-transport-validation-20261109/`（构建输出目录，不入库）：

- `result.json`：逐组结果、解码数量、进程退出码与候选对检查。
- `selected-pairs.json`：从 Go 日志提取的实际选中候选对。
- `server.log`、各客户端日志：原始记录。
- `artifacts.json`：Go 源码基线和服务端、四个示例二进制 SHA-256。

另外通过示例编译、新增头文件 clang-format 20.1.8 检查及 `git diff --check`。
