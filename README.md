# livekit-sdk-cpp

C++17 LiveKit 收发 SDK。新增同步接口 `client::Room` 只处理已编码媒体与房间状态；`client::MediaEngine` 在 Room 上用 GraphEngine 管理采集、编解码、预览、播放和录制。接口与线程规则见 [MediaEngine](docs/media-engine.md)。

新接口的 connect 返回 bool，有值操作返回 Result<T>，void 控制直接记录错误日志；保留 Observer，不使用操作完成回调。SDK 原生示例和 livekit-app 的会议客户端已迁移到新接口。

旧 RtcEngine / RoomEngine 继续供尚未迁移的应用使用，其异步契约见 [完成回调与持续 Observer](docs/rtc-simple-observer.md)。下文涉及 rtc::、RoomServices、自定义图和共享采集的说明属于这套旧接口。

数据视图和线程取消类型使用 `livekit::compat` 命名空间，见 [C++17 迁移说明](../docs/development/cxx17-migration.md)。从旧版本升级时需重新编译 SDK 与使用方。

## 入口

| 使用方式 | 公共头文件 | 链接目标 |
| --- | --- | --- |
| 同步采集、编解码、预览、播放、录制 | livekit/media_engine.hpp | LiveKit::MediaEngine |
| 同步入会与编码帧收发 | livekit/client/room.hpp | LiveKit::Client |
| 旧接口：配置采集、编解码、预览、播放、录制与共享采集 | livekit/rtc/engine.hpp | LiveKit::RTCMediaEngine |
| 只入会、收发编码帧 | livekit/rtc/room_engine.hpp、livekit.hpp | LiveKit::RTCLiveKit |
| 统一入口：编码传输与高级 GraphEngine 扩展 | livekit/rtc/engine.hpp、graph_api.hpp | LiveKit::RTCEngine |
| 自行组合发布/订阅节点 | livekit/rtc/publish_node.hpp、subscribe_node.hpp | LiveKit::RTCNodes |
| 上层 VP8/H264/H265/Opus 编解码 | livekit/rtc/video_nodes.hpp、audio_nodes.hpp | LiveKit::RTCMedia |
| 应用供帧、文件、设备输入图 | livekit/rtc/input_node.hpp | LiveKit::RTCInputs |
| 混音输出、逐轨录制 | livekit/rtc/audio_output.hpp、recording_node.hpp | LiveKit::RTCOutputs |
| Token 与可信服务端管理 API | livekit/auth.hpp、api.hpp | LiveKit::ServerSDK |
| 本地文件播放 / 直播合成 | livekit/player.hpp、studio.hpp | LiveKit::Player / LiveKit::Studio |

当前 [编码媒体 API](docs/rtc-encoded-api.md) 包含后端直接收发、图组合、生命周期、静音、订阅需求、权限及线程契约。后端不依赖实际编解码器或采集、渲染设备。

普通 Session 的入口、共享源示例及所有权规则见 [媒体引擎 API](docs/rtc-media-engine.md)。

本版按配置选择视频 codec；自动 H265 → H264 重发布策略随复杂控制调度一起移除。H264/H265 codec 节点本身继续保留。

    输入 → 编码节点 → 发布节点 ─┐
    输入 → 编码节点 → 发布节点 ─┼─ 共享 RoomEngine / LiveKit 连接
    显示 ← 解码节点 ← 订阅节点 ─┤
    录制 ← 编码帧   ← 订阅节点 ─┘

轨道以稳定本地 TrackId 路由，服务端 SID 是当前会话绑定。每轨拥有独立队列、预算、反馈和关闭屏障。停止一条轨道不退出房间。已有编码帧可直接写入 PublishedTrack；SubscribedTrack::read() 返回拥有数据的完整编码帧。

RtcEngine 独占 RoomEngine，以两个 map 保存发布和订阅媒体图。控制 Completion 与 Observer 在 notification 队列执行，媒体输出使用独立有界 dispatcher。leave/close 是外部拥有者调用的同步屏障，close 是 leave 的别名，返回后可重新 join。回调不能阻塞等待其他回调、退会或销毁引擎；客户端负责 UI 线程交接。当前职责与所有权图见 [架构说明](docs/architecture.md)。

## 旧 RTC 后端目录

公共入口为 `rtc::engine::RtcEngine`，头文件为 `livekit/rtc/engine.hpp`，状态和逻辑直接定义在该类中。
RoomEngine 的公共头文件为 `livekit/rtc/room_engine.hpp`，可注入 RoomTransport 工厂做无窗口测试；普通应用使用 `create_livekit_engine()`。
RoomEngine 通过 RoomTransport 发起协议请求；单次结果经 Completion、持续事件经 Observer 传给 RtcEngine，两者均经过 notification 队列。
详细边界见 [房间状态机设计](docs/rtc-room-state-machine.md)。

```text
src/backends/
├── rtc/
│   ├── room_engine.cpp       # control 队列上的房间与端点状态
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
| LIVEKIT_BUILD_CLIENT | 新同步 Room；同时启用 RTC_MEDIA 时提供 MediaEngine |
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

- room_client / basic_room_encoded：使用新 Room 入会、直接收发编码帧。
- automatic_session：使用同步 MediaEngine 发布合成音频、按成员快照订阅。
- video_source / audio_source：使用同步 MediaEngine，输入 → codec → Room。
- video_stream / audio_stream：使用同步 MediaEngine，Room → codec → 应用消费；设备播放显式启用。
- CLI lk：[命令说明](docs/cli.md)，支持多流、录制及逐轨统计。
- conference_viewer：每轨图、动态发布、静音、质量和录制。
- livekit-app 的会议客户端使用新 MediaEngine；投屏、远控及直播工作台保留现有引擎。

纯传输代码不隐式创建采集、音频设备或呈现窗口。程序测试使用合成输入、无窗口渲染和独立 SFU；自动化结果不代表物理设备人工验收。

## 当前能力边界

支持多条独立音视频轨、VP8/Opus 编码传输及可选 H264/H265 上层 codec、完整重新入会、逐轨静音/订阅需求/发布权限和会话服务。

原生 RTC 默认开启可协商的 FlexFEC 视频保护，可通过 `LiveKitRoomOptions::enable_flexfec` 关闭；对端需支持该能力。SDK 测试方式和当前 SFU 限制见 [FlexFEC 说明](docs/rtc-flexfec.md)。

单 PC、信令 resume/无缝房间迁移、simulcast/SVC 尚未实现。媒体模块新增 APM、NetEq 抖动缓冲/PLC 和基于发送端采集时间的音画同步；缺少共同发送端时钟的轨道不宣称同步。本轮 Windows 自动化验证与限制见 [重构记录](docs/architecture-refactor-20261029.md)；尚未完成真实设备人工验收。多流不等同于同一轨道多编码层。具体已验证范围与限制以 [实施记录](docs/rtc-backend-refactor.md) 为准。

历史接口文档和旧验收记录中的 Room API 示例不能用于当前版本。
