# 旧引擎移除与迁移

2026-11-03：删除旧公共 `rtc::engine::RtcEngine`、`rtc::RoomEngine` 和其底层实现。没有保留旧 API 别名或隐藏的旧构建目标。

删除范围包括 `src/engines/rtc`、`src/backends/rtc`、旧 `src/backends/webrtc` 编码传输、发布/订阅图节点、RoomServices、异步完成回调及阻塞桥。旧实现专用单测和集成入口随实现移除；保留通用 Result、媒体源、重采样、服务端 API、编解码与新 Room/MediaEngine 测试。历史需求报告处理函数仍可读取历史证据，旧 UAV 可执行程序入口已移除。

共享 FFmpeg codec 和 PCM 重采样器迁到 `src/media_nodes/codecs`，媒体输出和同步节点迁到 `src/engines/media`，回调上下文迁到 `src/media_graph`，通用事件队列迁到 `src/support`。`include/livekit/rtc` 中剩余文件是媒体节点和值类型，不再包含旧房间引擎。

| 原调用方式 | 当前方式 |
| --- | --- |
| create_livekit_engine / RtcEngine | client::MediaEngine，链接 LiveKit::MediaEngine |
| create_livekit_room / RoomEngine | client::Room，链接 LiveKit::Client |
| join + Completion | connect(ConnectionOptions)，同步返回 bool |
| publish_video / publish_audio | publish_media(MediaPublishSpec)，返回 Result<MediaPublication> |
| remote_track_added 描述符 | remote_participants → track_publications → MediaSubscribeSpec.publication |
| unpublish / unsubscribe / leave | unpublish_track / unsubscribe_track / close，返回 void |
| RoomServices::send_data | MediaEngine::publish_data 或 LocalParticipant::publish_data |
| statistics 事件 | statistics() 同步获取 PeerStatistics |
| Completion + Observer | 同步控制 + RoomObserver |

SDK CLI、全部 SDK 示例和会议、投屏、远控、直播工作台都已迁移。应用工作线程执行同步控制，媒体回调只写有界队列。远控仍绑定对端 identity 与会话 SID，重连、对端离开或 SID 替换需重新授权；发送成功只代表本地提交，交付以应用协议 ACK/看门狗判断。

MediaTrackSnapshot 的 frames/bytes 是编码媒体边界计数，input 提供输入完成状态，CLI 使用它保留文件播完退出行为。

旧 RPC、流、数据轨、E2EE、区域选择、异步 TokenProvider、自定义发布/订阅图 API 和共享采集句柄未在新接口提供。删除旧底层不表示这些能力已经迁移。新 Room 内部的 `src/backends/livekit/rtc_engine.*` 属于 Rust 风格会话恢复层，继续保留；它不是被移除的公共 RtcEngine。

验证使用编译、契约测试、无窗口测试和本机隔离 SFU，不代表真实设备人工验收。

## 本次验证（Windows x64 / Release）

- SDK 全量构建通过，包含新 Room/MediaEngine、CLI、示例、Player/Studio 库和测试。
- 会议、投屏、远控、直播工作台及其测试构建通过。独立播放器桌面应用未参与本轮构建，本机 Qt 缺少 QtSvg。
- SDK CTest：30 项通过，1 项因未设置外部 SFU 环境跳过（livekit_client_integration）。
- 应用 CTest：22 项通过，2 项因未设置 SFU 环境跳过（remote_media、studio_media_session）；这两项已分别由隔离 SFU runner 执行通过。
- CLI 隔离 SFU 集成通过：Token、房间 API、非法输入、VP8/Opus、数据包、录制、Unicode 文件路径、多文件发布、EOF 退出与关闭。
- 媒体图集成通过：GraphEngine 编码、Room 端口、解码、无窗口播放和录制。
- 远控集成通过：合成屏幕解码、双向定向数据、对端离开撤销授权。直播集成通过：双端音视频、订阅、统计与对端清理。
- 旧引擎文件/头文件/构建目标引用检查、235 个 SDK 原生文件的禁异常契约检查和 git diff --check 通过。

常规回归命令（从聚合仓库根目录执行）：

```powershell
ctest --test-dir livekit-sdk-cpp/out/windows-x64 -C Release --output-on-failure -j 2
ctest --test-dir livekit-app/out/rtc-products-qt6 -C Release --output-on-failure -j 2
```

远控测试目标沿用 SDK 同源测试的禁异常编译设置，避免混用异常配置导致非法 JSON 测试崩溃。投屏明确关闭自动订阅，只发布本地屏幕。

以上均为自动化结果，未进行真实设备人工验收，也未验证 macOS/Linux 构建。
