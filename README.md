# livekit-sdk-cpp

C++17 LiveKit 收发 SDK。新增同步接口 `client::Room` 只处理已编码媒体与房间状态；`client::MediaEngine` 在 Room 上用 GraphEngine 管理采集、编解码、预览、播放和录制。接口与线程规则见 [MediaEngine](docs/media-engine.md)。

新接口的 connect 返回 bool，有值操作返回 Result<T>，void 控制直接记录错误日志；保留 Observer，不使用操作完成回调。SDK 原生示例和 livekit-app 的会议客户端已迁移到新接口。

旧公共 RtcEngine / RoomEngine、RoomServices、完成回调及其传输/信令实现已删除，不提供兼容别名。迁移方式与被移除能力见 [迁移说明](docs/legacy-engine-removal.md)。

## 入口

| 使用方式 | 公共头文件 | 链接目标 |
| --- | --- | --- |
| 同步采集、编解码、预览、播放、录制 | livekit/media_engine.hpp | LiveKit::MediaEngine |
| 同步入会与编码帧收发 | livekit/client/room.hpp | LiveKit::Client |
| VP8/H264/H265/Opus 编解码节点 | livekit/rtc/video_nodes.hpp、audio_nodes.hpp | LiveKit::RTCMedia |
| 文件、设备、应用供帧输入节点 | livekit/rtc/input_node.hpp | LiveKit::RTCInputs |
| 混音输出、逐轨录制节点 | livekit/rtc/audio_output.hpp、recording_node.hpp | LiveKit::RTCOutputs |
| Token 与可信服务端管理 API | livekit/auth.hpp、api.hpp | LiveKit::ServerSDK |
| 本地文件播放 / 直播合成 | livekit/player.hpp、studio.hpp | LiveKit::Player / LiveKit::Studio |

MediaEngine 独占 Room 和各轨道媒体图。Room 只处理编码帧、参与者、信令和传输，不依赖 GraphEngine。共享采集/编解码/输出节点仍在媒体层。详细边界见 [架构说明](docs/architecture.md)。

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
| LIVEKIT_BUILD_RTC_GRAPH_NODES | 媒体图节点 API |
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
- livekit-app 的会议、投屏、远控、直播工作台均使用新 MediaEngine。

纯传输代码不隐式创建采集、音频设备或呈现窗口。程序测试使用合成输入、无窗口渲染和独立 SFU；自动化结果不代表物理设备人工验收。

## 当前能力边界

支持独立音视频轨、编码帧收发、逐轨静音/订阅需求、数据包、成员快照和完整重新入会。MediaEngine 支持 VP8/Opus 及可选 FFmpeg H264/H265 编解码、录制与媒体输出。

旧 RoomServices 的 RPC、流、数据轨和 E2EE 没有在新接口中实现；单 PC、信令 resume、无缝迁移及 simulcast/SVC 也未提供。当前可用接口以 [MediaEngine](docs/media-engine.md) 和 [Room 设计](docs/livekit-client-design.md) 为准。历史接口文档和验收记录不代表当前版本的能力。
