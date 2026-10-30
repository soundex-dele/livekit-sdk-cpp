# livekit-sdk-cpp

当前分层、帧契约与迁移说明见 [架构说明](docs/architecture.md)。最新的 [源节点分发与任务队列设计](docs/source-driven-runtime.md) 已移除 RtcEngine/pipeline 的 poll 接口。

已实现的 [RTC 同步控制与 Result 重构方案](docs/rtc-sync-result-refactor-20261029.md) 采用操作锁串行化、调用线程执行和同步 Result，不引入控制任务队列；入会与离会互相等待，不抢占打断。验证记录见 [实施验证](docs/rtc-sync-result-validation-20261029.md)。

C++20 LiveKit 收发 SDK。RoomEngine 负责入会、信令、已编码媒体传输及会话服务；发布和订阅分别是 GraphEngine 的 sink/source 节点。RtcEngine 是统一入口，协调 RoomEngine、可选媒体模块和每轨 GraphEngine 流水线，多路音视频共享一个房间连接。

普通媒体会话通过 RtcEngine 装配可选的内部 RtcMediaEngine：Session 提交配置或应用数据源，引擎创建采集、编解码、预览、播放和录制图。共享采集源可供多个发布或多个房间复用，各路保留独立编码器与队列。

旧 Room、Participant、TrackPublication、VideoFrameStream、AudioFrameStream API 和 LiveKit::SDK 目标已移除，没有兼容层。调用方与测试入口已迁移，支持边界与验证记录见 [实施清单](docs/rtc-backend-refactor.md)。

旧公开 RtcMediaEngine / RoomSession 已移除，统一入口与退房重入契约见 [RTC 引擎 API](docs/rtc-media-engine.md)。

## 入口

| 使用方式 | 公共头文件 | 链接目标 |
| --- | --- | --- |
| 配置采集、编解码、预览、播放、录制与共享采集 | livekit/rtc/engine.hpp | LiveKit::RTCMediaEngine |
| 只入会、收发编码帧 | livekit/rtc/livekit.hpp | LiveKit::RTCLiveKit |
| 统一入口：编码传输与高级 GraphEngine 扩展 | livekit/rtc/engine.hpp、graph_api.hpp | LiveKit::RTCEngine |
| 自行组合发布/订阅节点 | livekit/rtc/publish_node.hpp、subscribe_node.hpp | LiveKit::RTCNodes |
| 上层 VP8/H264/H265/Opus 编解码 | livekit/rtc/video_nodes.hpp、audio_nodes.hpp | LiveKit::RTCMedia |
| 应用供帧、文件、设备输入图 | livekit/rtc/input_node.hpp | LiveKit::RTCInputs |
| 混音输出、逐轨录制 | livekit/rtc/audio_output.hpp、recording_node.hpp | LiveKit::RTCOutputs |
| Token 与可信服务端管理 API | livekit/auth.hpp、api.hpp | LiveKit::ServerSDK |
| 本地文件播放 / 直播合成 | livekit/player.hpp、studio.hpp | LiveKit::Player / LiveKit::Studio |

完整的 [编码媒体 API](docs/rtc-encoded-api.md) 包含后端直接收发、图组合、生命周期、静音、订阅需求、权限及线程契约。后端不依赖实际编解码器或采集、渲染设备。

普通 Session 的入口、共享源示例及所有权规则见 [媒体引擎 API](docs/rtc-media-engine.md)。

    输入 → 编码节点 → 发布节点 ─┐
    输入 → 编码节点 → 发布节点 ─┼─ 共享 RoomEngine / LiveKit 连接
    显示 ← 解码节点 ← 订阅节点 ─┤
    录制 ← 编码帧   ← 订阅节点 ─┘

轨道以稳定本地 TrackId 路由，服务端 SID 是当前会话绑定。每轨拥有独立队列、预算、反馈和关闭屏障。停止一条轨道不退出房间。已有编码帧可直接写入 PublishedTrack；SubscribedTrack::read() 返回拥有数据的完整编码帧。

RtcEngine 在调用线程上通过共享会话操作锁串行处理控制与建图，返回完成的 Result，不再提供 poll()/next_event()/next_failure()，房间事件和图错误通过 set_callbacks() 通知；源节点在自己的线程上等待并分发媒体与反馈；采集、编解码及设备输出放在各自有界工作线程。RoomServices 在同一 backend 上提供可靠/不可靠数据、RPC、文本/字节流、DataTrack、DTMF、转写及加密。UI 使用不可变快照。

## RTC 后端目录

公共房间控制器为 `rtc::RoomEngine`，头文件为 `livekit/rtc/room_engine.hpp`。
`RtcEngine` 持有会话，`RoomEngine` 通过内部 `RoomTransport` 接口使用具体适配器。
`create_livekit_room()` 返回 `Result<std::unique_ptr<RoomEngine>>`。

```text
src/backends/
├── rtc/
│   ├── room_engine.cpp       # 房间生命周期、轨道、事件和操作队列
│   ├── room_transport.hpp     # 内部适配接口
│   ├── endpoint.cpp
│   ├── queue_budget.hpp
│   ├── livekit/               # LiveKit 信令、传输适配与专属服务
│   └── mediasoup/             # 扩展说明，尚无实现
├── webrtc/                    # PeerConnection、编码帧收发和传输运行时
├── ffmpeg/
└── platform/
```

`RoomBackend` / `room_backend.hpp` 已改名，不提供旧名称别名；调用方需更新类型和包含路径。
`LiveKit::RTCBackend` 仍是公共 RTC 后端模块的链接目标，其他 CMake 目标和构建开关保持不变。
LiveKit 数据消息、RPC、流、加密及重连策略实现在 `rtc/livekit/`；依赖 LiveKit
参与者与协议类型的帧加密适配器也属于此目录。WebRTC 底层实现不包含 LiveKit 协议头文件。

`RoomOptions` 仅包含通用房间设置与不可变后端配置。LiveKit 调用方使用
`LiveKitRoomOptions`，房间快照通过 `BackendCapabilities` 报告可选服务能力。
`mediasoup/` 尚无实现。

## 构建

使用同级的 webrtc M137 CMake checkout 和 graphflow-cpp。Windows 需要 VS2022、Windows SDK、CMake、NASM、Perl、vcpkg 的 OpenSSL/libvpx/dav1d。SDK 默认 /MT；Qt 产品用 /MD，预编译 WebRTC 与依赖必须匹配 CRT。首次构建会下载固定版本依赖。

    cmake -S . -B out/rtc -A x64 -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
    cmake --build out/rtc --config Release --parallel
    ctest --test-dir out/rtc -C Release --output-on-failure

按层选择构建：

| 选项 | 含义 |
| --- | --- |
| LIVEKIT_BUILD_RTC_NATIVE | LiveKit/WebRTC 编码媒体传输 |
| LIVEKIT_BUILD_RTC_GRAPH_NODES | 发布/订阅节点及流水线 |
| LIVEKIT_BUILD_RTC_MEDIA | 可选媒体引擎及实际 codec、输入、输出节点；需要前两项 |
| LIVEKIT_BUILD_SERVER_SDK | 可信服务端管理 API |
| LIVEKIT_BUILD_PLAYER | 独立 FFmpeg 播放器 |
| LIVEKIT_BUILD_STUDIO | Studio 合成/混音；需要 MEDIA 和 PLAYER |
| LIVEKIT_BUILD_CLI / DEMO / EXAMPLES / TESTS | CLI、会议示例、公共示例、测试 |
| LIVEKIT_RTC_FFMPEG_CODECS | 可选 H264/H265 软件 codec，需要显式 FFmpeg 开发包 |

只构建传输时关闭 GRAPH_NODES、MEDIA、CLI、DEMO、EXAMPLES；只构建后端状态机单测时再关闭 NATIVE 和 SERVER_SDK。不再使用 LIVEKIT_BUILD_RTC 开关。

不含 RTC 的播放器或 ServerSDK 构建必须显式关闭 NATIVE、GRAPH_NODES、MEDIA、CLI、DEMO、EXAMPLES；按需启用 PLAYER 或 SERVER_SDK。

## 示例和产品

- room_client：入会、房间快照、RoomServices 数据。
- video_source / audio_source：输入 → codec → 发布节点。
- video_stream / audio_stream：订阅 → codec → 应用消费；设备播放显式启用。
- CLI lk：[命令说明](docs/cli.md)，支持多流、录制及逐轨统计。
- conference_viewer：每轨图、动态发布、静音、质量和录制。
- livekit-app 中的会议、投屏、远控及直播工作台均由 Session 提交配置，RtcMediaEngine 负责构图。

纯传输代码不隐式创建采集、音频设备或呈现窗口。程序测试使用合成输入、无窗口渲染和独立 SFU；自动化结果不代表物理设备人工验收。

## 当前能力边界

支持多条独立音视频轨、VP8/Opus 编码传输及可选 H264/H265 上层 codec、完整重新入会、逐轨静音/订阅需求/发布权限和会话服务。

单 PC、信令 resume/无缝房间迁移、simulcast/SVC 尚未实现。媒体模块新增 APM、NetEq 抖动缓冲/PLC 和基于发送端采集时间的音画同步；缺少共同发送端时钟的轨道不宣称同步。本轮 Windows 自动化验证与限制见 [重构记录](docs/architecture-refactor-20261029.md)；尚未完成真实设备人工验收。多流不等同于同一轨道多编码层。具体已验证范围与限制以 [实施记录](docs/rtc-backend-refactor.md) 为准。

历史接口文档和旧验收记录中的 Room API 示例不能用于当前版本。
