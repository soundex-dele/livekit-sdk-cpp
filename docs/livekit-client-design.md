# LiveKit 同步编码帧客户端设计与实现

更新：2026-11-02。按 Rust SDK 的职责重新实现，已完成 Windows Release 编译与无窗口自动化验证。

## 约定

- C++17；实现位于 `src/backends/livekit/`，公共命名空间 `livekit::sdk::client`。
- 公共操作同步返回，不提供完成回调、future 或 coroutine；保留 `RoomObserver` 持续事件。
- `connect()` 返回 bool；创建 Track、发布 Track 返回 `Result<T>`；读写返回专用状态。
- 无返回值的公开操作使用 void，失败在操作所在函数直接调用 `LOG_ERROR`。不包装日志函数。
- 日志使用从 `graphflow-cpp/utils/logger.h` 复制的 `src/support/logger.h`；仅链接 utoolkit logging。
- 输入和输出为完整编码访问单元。不使用 GraphEngine，不进行采集、实际编码、解码、播放。
- 上述同步、日志和媒体边界遵循用户明确要求，覆盖仓库默认的错误返回和 GraphEngine 建议。

## 与 Rust 的职责对应

| Rust 实现 | 新 C++ 实现 | 实际职责 |
| --- | --- | --- |
| room/mod.rs 的 Room / RoomSession | room.cpp、room_session.cpp | 房间生命周期、成员列表协调、Observer 分发 |
| participant/local_participant.rs | local_participant.cpp / ParticipantData | AddTrack → sender → Publication；撤销发布；重新发布；数据和成员信息操作 |
| publication/remote.rs | track_publication.cpp / PublicationData | 订阅意图、订阅/取消、接收需求、订阅状态 |
| track 模块 | track.cpp / TrackData | 编码帧入口、读取队列、背压、epoch、静音和反馈 |
| rtc_engine/mod.rs | rtc_engine.cpp | 单一控制线程、同步请求桥接、连接失败和全量重连调度 |
| rtc_engine/rtc_session.rs | rtc_session.cpp | 信令请求、发布确认、SDP 协商、原生 sender、数据通道 |
| rtc_engine/peer_transport.rs | peer_transport.cpp | 直接持有 WebRTC PeerConnection；SDP、ICE、receiver、DataChannel |
| livekit-api 的 signal client | signal_client.cpp | WebSocket 和 Protobuf 消息，收发容量限制 |

RoomSession 不再拥有 publish/unpublish/mute/data 等业务操作的实现。成员拥有自己的 publications；Publication 拥有订阅意图；Track 拥有媒体状态。RtcEngine 管恢复，RtcSession 管一次底层连接。

这次不是把旧 RoomTransport 改名。新实现不包含或链接旧 RoomEngine、RoomTransport、Endpoint、NativePeer、SignalSession 或 RTCBackend。旧后端文件和构建关系恢复原样。公共值类型也不再 alias 到 `rtc::*`。

`media/` 保留并适配了原有已编码帧注入 WebRTC 的底层算法，包括 Opus transformer、视频 packet envelope、codec 元数据和预算计数；这些代码使用新客户端自己的值类型和生命周期，不调用旧传输对象。视频 envelope 的 Encode 接口只交付已有编码数据；音频 envelope 不编码 PCM。

## 公共接口

```cpp
Room room;
room.set_observer(&observer);  // observer 的生命周期覆盖注册区间
if (!room.connect(options)) { return; }
auto participant = room.local_participant();
auto track = LocalTrack::create_audio("encoded-opus", opus_format);
if (!track) { return; }
auto publication = participant.publish_track(*track);
if (!publication) { return; }
EncodedFrame frame;
frame.buffer = EncodedBuffer(encoded_access_unit);
frame.epoch = track->feedback().epoch;
frame.timestamp = timestamp_in_codec_clock;
frame.duration = std::chrono::milliseconds(20);
auto status = track->try_write(std::move(frame));
participant.unpublish_track(publication->snapshot().sid);
room.close();
```

- Room：connect、close、set_observer、connection_state、info、local_participant、remote_participants。
- LocalParticipant：publish_track、unpublish_track、publish_data、update、snapshot、track_publications。
- LocalTrack：create_audio/create_video、try_write、set_muted、snapshot、feedback。
- RemoteTrackPublication：set_subscribed、set_receive_options、snapshot、track。
- RemoteTrack：read、request_keyframe、snapshot。

旧的公开 `Room(TransportFactory)` 删除。测试通过内部 RtcSession 边界注入 fake session，生产 API 不暴露传输工厂。

## 连接和同步语义

Rust 的 `Room::connect().await` 在加入房间并等待所需 PeerConnection Connected 后才返回成功。新实现同样不把 WebSocket open 或 JoinResponse 当作 connect 成功。

新客户端建立 publisher/subscriber 两个 PeerConnection，并建立 publisher 数据通道。publisher 协商完成后，根据 JoinResponse.subscriber_primary 等待主连接；初始化 roster 后返回 true。鉴权失败、网络错误、超时或 close 取消都返回 false 并记录日志。

publish_track 等待服务端 AddTrack 确认、创建原生 sender、完成 SDP 协商后返回 Publication。set_subscribed(true) 等待 receiver 绑定；false 完成本地解绑和订阅请求提交。mute 和接收需求没有服务端操作 ACK，同步返回表示本地应用/提交完成。publish_data 等待数据通道接受数据，不等于远端应用已收到。update 使用协议 15 的 request_response。

RtcEngine 的控制线程串行执行会话操作。WebRTC 内部依然使用其原生异步 observer，但不会向调用者暴露完成回调。操作使用共同 deadline 和 close 取消标志。

内部错误返回对齐 Rust：`EngineResult<T>` 对应 C++ `Result<T>`，`EngineResult<()>` 对应 `Result<void>`。RtcSession、RtcEngine、信令和 PeerTransport 的可失败操作显式返回成功或失败，布尔判断为真表示成功；不再用空 `Failure` 表示操作成功。`Failure` 仅作为错误详情，在结果、事件和状态快照间保留原始错误。close 等清理接口仍为 void。

这一规则适用于内部调用链。公共接口维持既定约定：connect 返回 bool，无业务返回值的控制接口返回 void，并在所在函数直接使用 LOG_ERROR 宏记录内部 Result 的失败；Observer 保留。

Observer 运行在独立通知线程。只允许快照查询；同步控制操作和阻塞 read 在 SDK 线程被拒绝，避免重入死锁。set_observer(nullptr) 等待正在执行的通知结束，返回后旧 observer 可以释放。销毁 Room 必须由外部所有者在其他公开调用返回后执行。

## 媒体和所有权

编码类型为 Opus、VP8、H264 Annex-B、H265 Annex-B；一次发布只有一个 encoding。codec/fmtp 是否可用在创建 sender 时验证。

每条 Track 有帧数、字节数、最大帧龄限制；整个 Room 有字节预算。预算随编码数据跨 SDK 和 WebRTC 队列的所有权移动，未发送完的数据仍占用预算。读出的数据离开 SDK 预算。写入要求有效 epoch、递增时间戳、正 duration 和对应 encoding_id。视频断链后等待关键帧。

同一 LocalTrack 只允许被一个发布对象持有；失败发布释放占用。RemoteTrack 是单读者队列；close、取消订阅和重连会唤醒阻塞 read。缓冲区持有不可变共享数据。

## 恢复

RtcEngine 在可重试连接中断后按有界退避创建新的 RtcSession；每次递增 epoch。LocalParticipant 重新发布自己的 Track，并更新原 Publication 的 SID；Track / Publication 句柄稳定。静音状态保留，旧 epoch 的输入被拒绝。

远端旧轨道结束；RoomSession 根据新的成员 roster 建立新 Publication。订阅意图和接收需求优先按 identity 和 SID 匹配；SID 改变时仅在 name/kind/source 唯一匹配时迁移，避免混淆同名轨道。

AddTrack 超时可能留下服务端未知结果；不存在针对未创建 sender 的 remove-track 消息，因此本实现触发全量 rejoin 来隔离迟到确认。不会把超时发布作为成功发布恢复。

## 当前范围与构建

构建目标 `LiveKit::Client`，开关 `LIVEKIT_BUILD_CLIENT`。无 native 构建支持公共 API 和 fake session 测试，真实连接要求 `LIVEKIT_BUILD_RTC_NATIVE=ON`。

直接依赖：WebRTC、协议生成库、ixwebsocket、JSON、Threads、utoolkit logging。自动检查源文件 include 和实际项目依赖闭包，禁止旧 RTC 后端和 GraphEngine 混入。

本轮范围是基础 Room 客户端，不宣称 Rust SDK 全功能等价。仅双 PC、单编码发布、全量重连；尚未提供保持 PC 的 signal resume、single-PC、simulcast、区域切换、TokenProvider、E2EE、RPC 和数据流扩展。普通 WebRTC DTLS/SRTP 仍由 WebRTC 执行。这些扩展需要在相应对象层加入，不应重新集中到 RoomSession。

示例 `examples/basic_room_encoded` 输入已编码 Opus 静音帧。集成测试使用独立本地 SFU；Opus 和收到的 VP8 仅在测试程序/FFmpeg 中解码。

## 本轮验证

代码调整完成后统一执行：无 native 契约测试、native 构建、头文件独立编译、依赖边界检查、旧 public_room 回归、真实 SFU 编码媒体和服务重启测试。

本轮结果（内部 Result 与 Rust 对齐后重新编译和验证）：

| 检查 | 结果 |
| --- | --- |
| 无 native Release 构建与测试 | 通过；client、client_dependencies、rtc_backend、rtc_dependency_boundaries，4/4 |
| native Release 构建 | client、integration、basic_room_encoded 编译通过；此前的 public_room 和公共头文件独立编译检查通过，本轮公共头文件未改动 |
| native 自动测试 | client、client_dependencies、public_room，3/3 |
| 源码与链接边界 | 无旧 RTC 后端、RoomTransport、Endpoint、NativePeer、SignalSession 或 GraphEngine 依赖 |
| Fake session 回归 | 同步完成、超时释放、关闭打断 connect/publish/read、背压、跨房间独占、Observer 重入拒绝/注销屏障、重连 SID/epoch、退订意图、失败后 Track 转移、20 次连接关闭 |
| Result 错误传播 | sender 失败保留原始 Failure 和后端原因，回滚发布并释放 Track；订阅失败写入快照，重试成功清除错误；编译期校验内部 Result<void> 与公共 bool/void 签名 |
| 实际 SFU | 150 帧 Opus 解码；74 帧 VP8 收到并全部由 FFmpeg 解码，红蓝内容匹配；数据消息到达 |
| 服务进程重启 | 自动全量重连；SID 更新、epoch=2；恢复后解码 20 帧 Opus、收到 14 帧视频 |
| 原生控制 | 静音、取消静音、音视频撤销发布通过 |
| 编解码边界 | 原生会话检测意外 PCM 编码/解码及视频解码调用；本轮收发未触发 |
| 日志 | 无效鉴权明确返回 false；宏输出 RoomSession::connect 的实际行号和 HTTP 401，无转发日志函数 |
| 格式 | 新增 C++ 文件 clang-format 20.1.8 检查通过；git diff --check 通过 |

本地集成验证输出：`out/livekit-result-alignment-integration/`（聚合仓库根目录下），含 result.json、client.log、接收 IVF 和解码 RGB。可用 `tests/livekit_client/run_integration.py --server <SFU> --client <integration.exe> --ffmpeg <ffmpeg> --output <目录> --restart` 复现。

所有验证为无窗口自动化；不代表摄像头、麦克风、扬声器等实际设备人工验收。
