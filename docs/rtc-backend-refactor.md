# RTC backend / nodes / engine 重构

状态：本次 backend / nodes / engine 分层迁移已完成，旧 Room API 已删除且不保留兼容层。当前恢复方式是完整重新入会；单 PC、信令 resume、无缝迁房、simulcast/SVC、AEC/APM、播放抖动缓冲/PLC 和跨轨同步不在已完成能力中。下方实施记录按时间保留，早期“仍待迁移”描述由末尾收尾记录取代。

## 目标边界

- `backends/rtc` 拥有房间、信令、发布订阅、传输恢复及轨道端点。公共契约不含 WebRTC、GraphEngine、像素、设备或播放器类型。
- `media_nodes/rtc` 的发布 sink 和订阅 source 分离；一条逻辑轨道对应一个节点，多节点共享 RoomEngine。停止节点不能退会。
- `engines/rtc` 创建 backend、节点和 GraphEngine 图，负责媒体源、编码器、录制与播放的组合策略。backend 不创建媒体图。
- 多流采用稳定本地 TrackId；服务端 SID/SSRC 是可替换绑定。编码层不是独立房间。每轨有界队列，会话有总字节预算，慢轨道不会阻塞其他轨道。
- 传输恢复由 backend 独占执行；engine 恢复媒体源。旧 epoch 数据、旧操作确认不能恢复已删除轨道。
- 默认以完整编码帧交接；底层不执行媒体编码、解码或渲染。RTP 扩展必须明确重封包语义，不能将诊断 packet observer 当作媒体端点。

## 实施与验收

- [x] 中立帧/描述/反馈、逐轨与会话预算、端点生命周期契约及单测。
- [x] 新 Room/信令实现下沉；WebRTC adapter 与 GraphEngine/设备/编解码分离（旧路径删除另列）。
- [x] 发布/订阅节点、engine 构图、多轨增删、关闭屏障。
- [x] 多轨发布、接收路由、背压、反馈、重连删除竞态及旧代次隔离测试。
- [x] 编码视频及编码音频实际收发，不依赖 PCM/真实 codec 触发网络发送。
- [x] 数据消息、RPC、流、DataTrack、加密、逐轨权限及完整重新入会迁移到 backend；其余恢复模式保留明确限制。
- [x] CLI、示例、四个 RTC 产品及相关测试迁移；删除旧 API/旧媒体内嵌路径。
- [x] CMake 独立 RTC 构建边界及公共头依赖检查。
- [x] 四个 RTC 产品编译、无窗口自动化、指定服务端/Go 互通回归与文档更新；实际设备验收单独保留。

硬件设备人工验收与自动化验证分别记录。此任务不使用桌面自动化。

## 实施记录

2026-10-28：检查到旧 Room runtime 直接创建 MediaPump、TrackSink、GraphEngine、ADM 和编解码工厂，RecoveryState 保存 MediaSpec。不能仅移动文件实现分层，需要分别迁移会话状态与媒体图所有权。现有编码音频输入仍依赖原生音频 cadence，必须在最终纯传输实现中移除这一依赖。

### 当前已实现的部分

- 新 `rtc::RoomEngine` 控制器、PublishedTrack/订阅端点、中立编码帧与格式描述；逐轨队列和会话总队列字节预算。底层事件用 `(id, epoch, incarnation, direction)` 区分旧绑定、重连及同名新轨道。
- 新 `RtcPublishNode` 与 `RtcSubscribeNode`；`PublishPipeline`、`SubscribePipeline` 使用真实 GraphEngine 边。新内部 `RtcEngine` 创建一个 backend 并管理多张轨道图，异步等待发布/订阅确认后创建图，独立移除轨道。
- 控制线程之外不调用原生 adapter；新图采用同步媒体边与 engine 轮询，无每帧无限异步投递。下游媒体节点应使用各自有界处理队列。
- SignalSession、SignalSocket、连接恢复/区域/Token 策略已从 engine/support 移至 `backends/rtc`。SignalingOptions 与 SignalRecoveryState 不再包含 MediaSpec；原生信令拆为 `LiveKit::RTCSignaling`，通用 wire 编码工具与 ServerSDK 分开。
- 新 `TransportRuntime` 使用显式的编码媒体适配器、原生线程和无时钟的 dummy ADM，不创建媒体图、设备采集、真实 codec 或渲染器。使用 `EnableMedia`，不会自动安装内置 codec/APM 工厂。
- 新 `NativePeer` 管理异步 SDP、ICE 队列、原生连接状态和接收轨道。按 MID/LiveKit stream ID 重新绑定远端 SID；稳定协商完成后刷新复用 receiver，避免重建已移除轨道。关闭时即使 WebRTC 丢弃 SDP observer，也会用明确的生命周期错误结束 future。
- 新 `EncodedAudioSender` 直接向原生 ChannelSend delegate 提交拥有数据的 Opus 帧；无需 PCM、Opus 编码时钟或先前编码帧。原生发送队列有帧数/字节上限、过期和关闭检查，仍经过 SendRtpAudio 的 RTP/RTCP 与加密路径。
- 新 `EncodedVideoSender` 将外部编码访问单元经原生 VideoEncoder 接口透传到 OnEncodedImage，不创建实际编码器。每轨独立反馈与有界队列，编码帧保留媒体时钟精度，PLI/FIR 可在没有后续输入帧时反馈。`EncodedReceiver` 在原生解包后交付拥有数据的编码访问单元，不调用原生解码回调。
- 新 `LiveKit::RTCNative` 可用 `LIVEKIT_BUILD_RTC=OFF`、`LIVEKIT_BUILD_RTC_NATIVE=ON`、`LIVEKIT_BUILD_RTC_GRAPH_NODES=OFF` 单独构建。此目标只链接中立 backend 和 WebRTC；它是原生传输组件库，实际 LiveKit adapter 由后续新增的 `LiveKit::RTCLiveKit` 提供。

### 已执行验证

- 独立配置 `out/rtc-backend-contracts`：RTC 原生库、ServerSDK、Player、CLI、Demo 均关闭，新节点开启。`rtc_backend` 与 `rtc_pipelines` 两项 CTest 通过（最终 engine 版本 0.75 秒）。
- backend 测试使用 FakeTransport，覆盖独立视频流发送、共享连接、总预算、背压依赖链、接收路由、无新帧时 PLI、关闭唤醒、payload 生命周期、恢复删除意图、旧 ACK 补偿、同名轨道实例隔离、待确认发布取消、单轨失败隔离。
- pipeline 测试实际创建 GraphEngine，覆盖媒体/反馈边、启停准入、单图关闭不退会、多图 engine 构建和取消。
- 独立信令 target/wire 工具抽取后的 SDK 编译与回归通过（10.00 秒）。本轮最终版本 `out/windows-x64` 的 `rtc_pipelines`、`rtc_native`、`sdk` 三项回归全部通过（11.86 秒）。构建日志：`out/rtc-refactor-final-native-build.log`。
- 新 `rtc_native` 使用两个实际 PeerConnection，经 ICE/DTLS/SRTP 做本机无窗口回环。已通过双音频轨道和 VP8/H.264/H.265 双视频轨道收发、单轨移除不影响另一轨、同一 receiver 的 SID 重绑定、无后续帧的 PLI 反馈、SDP 操作中途关闭测试。音频字节和时间戳保持一致；H26x 比较 NAL 内容，允许 RTP 解包恢复不同长度的 Annex-B 起始码；视频保留帧间 3001 tick 的精度（原生 RTP 可随机化时间戳起点）。
- 原生队列测试覆盖实际 ChannelSend delegate 的容量、payload 所有权、旧 epoch、队列中过期以及源关闭后的丢弃。媒体边界计数器验证 PCM 编码/解码和视频解码调用均为零。
- 独立 `out/rtc-native-contracts` 关闭 GraphEngine、旧 SDK、Player、ServerSDK，复用本仓库匹配的 WebRTC 静态库：本轮最终版本 backend 和原生回环两项测试全部通过（2.33 秒）。构建日志：`out/rtc-refactor-isolated-build.log`。

原生回环经过实际本机网络传输；本轮新增的 SFU 验证记录见下文，独立 Go 客户端及真实设备仍未验收。模拟传输、原生回环和设备人工验收是不同的验证层级；这些通过项不能算整个 Room 迁移完成。

### 本轮新增进展

- 新 `LiveKit::RTCLiveKit` 和 `create_livekit_room()` 已接通 SignalSession、双 NativePeer、发布确认、发现和编码媒体路由，不经过旧 Room。
- `engine::create_livekit_engine()` 创建真实后端及独立发布/订阅图。公开头位于 `include/livekit/rtc`；房间示例已迁移到新接口，参见 `rtc-encoded-api.md`。
- QueueBudget reservation 随访问单元在端点和原生队列间转交，不重复预留；原生发送完成后释放。应用读出后的载荷不继续占用 SDK 队列预算。
- 取消发布等待 session worker 完成本地解绑；重新订阅创建新的接收队列；订阅 ACK 与路由更新原子发布；远端轨道移除关闭对应订阅。
- 关键帧请求与码率反馈分开处理，防止持续反馈反复清空已排队的恢复帧。
- 三项自动化回归 `rtc_backend`、`rtc_pipelines`、`rtc_native` 通过（2.78 秒）。新增满预算转交、原生发送持有总预算及反馈恢复帧测试。
- 实际 SFU 后端集成通过：双 Opus、双 VP8、载荷比较、快速重新订阅、独立关闭和待确认发布取消。报告：仓库 `out/rtc-livekit-integration/20261028-212840/result.json`。
- 实际 SFU Engine 集成通过：四张发布/订阅图及独立关闭后的剩余流收发。报告：`out/rtc-livekit-integration/20261028-212901-xaazn3r2/result.json`。
- 服务端为本机已有 `out/uav-sfu-go/livekit-server.exe`，版本输出 1.13.10；未将其称为官方发行包。测试使用独立端口、临时配置及无窗口进程。快速再订阅出现的确切 VP8 8×8 清屏帧已对照本机服务端源码确认，测试不跳过其他载荷差异。

- 独立配置 `out/rtc-native-contracts` 已成功构建完整 LiveKit adapter，关闭旧 SDK、GraphEngine、ServerSDK 和 Player。backend/native 两项测试通过（2.32 秒），完整 SFU 测试通过：`out/rtc-livekit-integration/20261028-213305-r3i8lt6i/result.json`。
- RPC、StreamSession、DataTrackSession、加密和数据消息编码工具已从 `engines/rtc` 移入 `backends/rtc`，拆为无媒体依赖的 `LiveKit::RTCServices`。原有消费者已更新引用；这些服务尚待接入新 RoomEngine 的公开接口及原生数据通道。
- 新独立 `rtc_services` 测试覆盖 RPC、分块/压缩流、DataTrack 和加密，已通过（9.08 秒），无需旧 SDK/ServerSDK/GraphEngine。测试重用抽出的契约，避免维护重复实现。
- 生命周期修正后的后端/Engine SFU 最终复验通过：`20261028-213202-slvuwdwb`、`20261028-213202-gahp7vbk`，均位于仓库 `out/rtc-livekit-integration`。

### 数据服务、可组合媒体图和合并预算

- `RoomServices` 已接通原生可靠/不可靠数据通道，支持数据消息、DTMF、转写、RPC、文本/文件流和 DataTrack。发送取消、超时、断线、保留句柄关闭及队列容量都有独立测试；不再依赖旧 Room。
- 数据与媒体 E2EE 已接入新 backend。发送使用 RTP 封包前加密；接收在终端编码帧变换器中解密，避免进入 WebRTC 解码路径。错误密钥拒绝、修正密钥后恢复、四轨媒体和再次订阅均已通过 SFU 自动化。VP8 媒体加密仍明确拒绝。
- `client_extensions_integration` 已迁到新 backend，并在关闭旧 SDK/GraphEngine/ServerSDK 的构建中运行。Go 客户端 SDK revision 为 `743ab2d6b12ff24c2b2f9a0cc208c7afe8602571`，构建时工作树干净；测试在独立进程中往返 100,000 字节文本、300,000 字节文件、40,000 字节 DataTrack，并验证 Opus/H264/H265 加密向量及 H264 媒体回传。最新加密媒体报告：仓库 `out/extensions-media-e2ee/20261028-225214-gtdo3uaq/result.json`。接收 H264 另经 FFmpeg 解码检查。
- `publish_graph` / `subscribe_graph` 允许 factory 在 ACK 后创建同一每轨 GraphEngine 的额外节点。新增激活、生产者退出屏障和有界轮询契约；部分启动失败关闭轨道。关闭时不持有 engine 注册表锁或输入锁等待生产者，避免正在完成的 `write()` 与退出屏障死锁。
- 新 `LiveKit::RTCMedia` 的 `VideoCodecNode` 在每轨独立有界 worker 上实际编解码，和传输层完全分开。支持发送反馈、关键帧恢复、旧 epoch 丢弃、完整媒体时间戳和 I420 所有权。队列过载、慢消费者、新 epoch、错误输入和关闭均有无窗口测试。
- `video_source` / `video_stream` 示例已迁到新 engine 图。双 VP8 原始视频通过实际 SFU 编解码并核对像素，独立关闭不影响另一流；H264 + E2EE 同样通过。报告分别为 `out/rtc-livekit-integration/20261028-223301-c4dnyvgp/result.json` 和 `20261028-223503-u0twhwrw/result.json`。
- 独立 `out/rtc-media-contracts` 关闭旧 SDK、ServerSDK 和 Player，开启 backend、GraphEngine 与实际视频节点，成功编译两个迁移后的视频示例。合并预算及退出屏障最终版本的 backend/pipelines/video_nodes 三项回归通过（1.40 秒）；实际双视频 SFU 图通过：`out/rtc-livekit-integration/20261028-225445-zqcsxqw1/result.json`。
- 每轨预算现在是房间预算的子额度，帧数和字节数随载荷贯穿端点与原生队列，native completion 才释放发送额度。已绑定的接收队列使用同一个子额度。`TrackSnapshot::outstanding_*` 报告两层持有量，`queued_*` 仍报告端点队列。满预算交接、原生占额、其他轨道独立准入、释放后继续准入已测试。
- 合并预算后的原生回环通过（1.82 秒）。最新四轨媒体 E2EE、重订阅、独立关闭和取消报告：`out/rtc-livekit-integration/20261028-225214-zxj50mmj/result.json`。
- 主构建已重编译受新服务 API 影响的旧 SDK；当时 backend/pipelines/native/services/video_nodes/sdk 六项回归全部通过（23.42 秒）。这是过渡期间的回归检查，不表示旧 Room 已被移除。后续修改的合并预算和退出屏障另有上述最终版本测试。
- 最终主构建重新编译并再次通过上述六项回归（22.38 秒）；包括新增的异常尺寸输入、解码像素输出字节限制。独立无旧 SDK 构建的 video_nodes 同样通过（0.41 秒）。最终 H264 加密双视频图报告：`out/rtc-livekit-integration/20261028-225939-mihnv9g0/result.json`。

所有报告使用本机指定 SFU 1.13.10，而非声称经过官方发行包认证；没有执行真实采集/播放设备人工验收。

### 音频图与会话状态迁移

- 新 `AudioCodecNode` 在独立有界 worker 上实现实际 Opus 编解码，支持单/双声道、PCM16/float32、44.1 kHz 等输入重采样、10 ms 输入聚合和独立媒体时间戳。旧 epoch、队列过载、坏包与时间缺口都有显式处理；不包含设备播放、NetEq、混音或音画同步。
- `audio_source` 和 `audio_stream` 已迁到 `publish_graph` / `subscribe_graph`，CMake 目标链接 `LiveKit::RTCMedia`，可关闭旧 SDK 编译。源示例按 10 ms 输入 44.1 kHz 浮点音频；接收示例把解码 PCM 交给有界应用队列。
- 音频节点无窗口测试通过：实际 Opus 往返、重采样、声道内容、32 位 RTP 回绕、过载、epoch 切换、无效 PCM 和坏包恢复。实际双音频图的明文与 E2EE 测试通过，关闭其中一路后另一流继续；报告：仓库 `out/rtc-livekit-integration/20261028-231619-7x2jktz7/result.json` 和 `20261028-231634-irovumvh/result.json`。接入新会话状态后的加密复验：`20261028-233406-vd5ka4eu/result.json`。
- 两个迁移后的音频示例作为独立进程通过本机 SFU 收发：源端编码 500 包，接收端取得每声道 96,000 样本。报告：`out/rtc-livekit-integration/20261028-232131-xa5_6_2b/result.json`。这是内存 PCM/网络验证，没有使用麦克风或扬声器。
- `RoomSnapshot::session` 新增不可变会话快照，包含房间、成员属性、公告轨道、静音标记、可选权限、发言状态和连接质量。快照通过后端事件队列交付，不依赖媒体图。状态提示可合并，避免发言状态更新耗尽生命周期事件队列。
- 新 `SessionRoster` 拒绝低版本更新和已离开 SID 的延迟复活；中断清空旧快照，新 JoinResponse 以新 epoch 安装。已持有的快照保留历史内容。修复本地 ParticipantUpdate 被错误解释成远端轨道发现的问题，并为远端发现补充 epoch 检查。
- protobuf 子集按仓库固定上游定义补入 `ParticipantInfo.version`、`permission` 和权限字段；协议字段契约通过。`rtc_session` 覆盖成员元数据/权限/静音变化、历史快照不变、断开后迟到消息、发言增量/全量和连接质量；backend 覆盖重连代次及状态提示合并。
- 会话快照实际 SFU 验证覆盖入会、成员加入、四路本地/远端公告、独立关闭与成员退出，明文及媒体 E2EE 都通过。报告：`out/rtc-livekit-integration/20261028-233007-1b8e9d05/result.json`、`20261028-233041-ldqazdo0/result.json`。
- 当时权限和静音增加的是状态观察；成员元数据及本地轨道静音的后续进展见下文。订阅权限策略仍需迁移。
- 主构建最终通过 backend、pipelines、native、session、services、audio_nodes、video_nodes、protocol_contract、sdk 九项回归（23.86 秒）。日志：SDK `out/rtc-audio-session-main-ctest.log`。旧 SDK 回归仅用于迁移期间检查现有消费者，不能算旧 API 已删除。

### 成员信息控制

- 新 `RoomEngine::update_local_participant` 更新自己的名称、元数据和属性，要求支持 ACK 的信令协议（15+），等待真实 `RequestResponse`。属性空值删除键，名称/元数据空值保持原值，不模拟协议不存在的清空语义。
- 后端限制单次更新 64 KiB 和 128 个待确认请求；校验 UTF-8、超时和属性数量。控制操作使用未复用的请求 ID 与 epoch，迟到 ACK、取消、超时、关闭及重连不会错误地完成下一次操作。已派发但没有确认的操作以 `outcome_unknown` 结束，不自动重放。
- 共享 UTF-8 检查抽到 `support/utf8.hpp`，中立 backend 无需依赖 protobuf/JSON。新增 FakeTransport 测试覆盖确认前 future 不完成、拒绝权限、过期、取消、重连、迟到确认、容量和关闭。
- 指定本机 SFU 的实际测试验证服务端 ACK、本地/远端名称与元数据更新、属性删除及无权限客户端被拒绝，并继续完成四轨收发回归。明文报告：仓库 `out/rtc-livekit-integration/20261028-234657-uak68s3e/result.json`。
- 同样的成员控制与四轨媒体 E2EE 组合测试通过，报告：`out/rtc-livekit-integration/20261028-234819-jh8713ff/result.json`。
- 最终主构建已重编译全部受影响的 backend、native、services、图节点与旧 SDK，九项回归全部通过（23.36 秒），日志：SDK `out/rtc-participant-control-final-ctest.log`。无旧 SDK 的独立 backend 控制测试通过（1.00 秒）。

### 逐轨静音控制（2026-10-29）

- `TrackDescriptor::muted` 支持初始静音发布；`PublishedTrack`、`RoomEngine` 和 `RtcEngine` 提供异步 `set_muted`。当前门控由 `TrackSnapshot::muted` 报告。端点保持稳定轨道身份及静音意图，重入时将当前意图写入新发布请求，旧句柄不能控制同名新轨道。
- 静音清空端点队列并使原生音视频发送队列中的旧代次失效，快速静音/恢复不能复活这些载荷。视频恢复要求关键帧；其他轨道保持独立。已进入传输协议栈的包和重传不能撤回。
- 音视频 codec 节点在活动状态或 `SenderFeedback::media_generation` 变化时清理旧输入/编码积累。该代次保证两次 engine 轮询间发生的完整静音/恢复仍可观察；带本地采集时间的旧编码帧在端点也会被过滤。
- 静音成功 future 表示本地原生门控加信令提交，不宣称收到服务端 ACK。MuteTrackRequest 没有独立请求 ID。后端接收服务端 mute 命令时也更新原生门控和端点；这一分支有 FakeTransport 覆盖。
- 独立构建的 backend/native/audio_nodes/video_nodes 四项测试通过（3.90 秒），覆盖缓存清理、快速切换、原生音视频门控、重新编码边界、初始静音、重入保持和旧句柄隔离。新增协议字段按固定上游验证通过。
- 实际 SFU 四轨测试验证初始静音、音频和视频静音后其他轨道继续、恢复收发与远端状态更新。明文报告：仓库 `out/rtc-livekit-integration/20261029-001416-9hjmlp1w/result.json`；媒体加密报告：`20261029-001520-m69lfc3x/result.json`。
- 明文测试观察到服务端静音填充包；已核对 `D:/workspace/livekit/pkg/sfu/downtrack.go` 的 `OpusSilenceFrame` 和 `writeBlankFrameRTP` 路径。测试只排除已静音轨道在切换后的确切 80 字节填充帧（f8 ff fe + 77 个零），不放宽其他载荷比较；E2EE 的 SIF 仍由解密器过滤。
- 实际原始视频 Engine 双图测试通过：保持两路源继续产帧，静音图不再准入编码，另一图继续收发；恢复后接收新的可解码画面。VP8 报告：`out/rtc-livekit-integration/20261029-001652-wu9v07eb/result.json`。

逐轨静音最终主构建的九项 CTest 全部通过（24.75 秒）。H.264 + E2EE 原始视频双图的静音恢复和独立关闭复验通过，报告：仓库 `out/rtc-livekit-integration/20261029-002745-zug9m9pw/result.json`。

### 轨道来源与订阅需求（2026-10-29）

- `TrackDescriptor::source` 使用 `PublicationSource` 显式区分摄像头、麦克风、屏幕和系统音频；unknown 保持 unknown，不再按媒体类型猜测。`source_id` 仍是来源身份，不代替轨道角色。发布时校验媒体类型，发现事件和成员快照保留服务端来源；重入保存发布意图。
- `SubscribedTrack::set_demand`、`RoomEngine::set_receive_demand`、`RtcEngine::set_receive_demand` 设置独立订阅的 SFU 转发需求，包含暂停、目标尺寸/帧率、质量及优先级。初始需求放在 descriptor，当前最后成功提交值位于 snapshot，重入使用后者。
- 需求更新 future 等待原生适配器提交信令；协议没有服务器 ACK。失败/未确定结果不替换恢复意图；迟到完成事件、旧句柄与新同名订阅隔离。该接口不暂停本地解码器或清除已在途媒体，服务端按可用编码层选择，不承诺转码到目标尺寸。
- 无旧 SDK 配置的 backend/pipelines 两项测试通过（1.89 秒），包括来源校验、恢复、需求提交前等待、失败/取消、音频参数校验、旧代次完成和旧句柄隔离。
- 实际指定 SFU 的四轨明文回归通过，验证四种来源、初始暂停、运行中暂停、持续生产时另一视频继续转发和恢复收发。报告：仓库 `out/rtc-livekit-integration/20261029-003717-_cu6_m1z/result.json`。
- 同样的四轨媒体加密回归通过：`out/rtc-livekit-integration/20261029-004033-c5323pht/result.json`。主构建九项 CTest 通过（24.15 秒）。

### 订阅权限迁移（2026-10-29）

- 新 `RoomOptions::subscription_permissions` 与 `RoomEngine::set_subscription_permissions` 支持初始及动态发布方允许列表。协议没有 ACK，future 成功表示本地提交；原生循环保留策略意图跨重入恢复，派发后的未知结果不会自动撤销该意图。
- 配置以 participant identity 和稳定的本地 publication ID 表达；在发布确认/移除时重新映射服务端 SID，初始限制在媒体发布前提交。未发布、已移除或未列出的 ID 不扩大授权。
- `subscription_permission_changed` 提供服务器的许可变化通知，包含发布者 SID 和轨道 SID；后端丢弃旧 epoch。等待绑定的订阅仍有操作超时，已经绑定的句柄保留，由服务器停止/恢复转发。
- 独立 backend/pipelines/session 三项自动化通过（2.18 秒）；覆盖无效策略、提交前等待、取消/中断、权限通知 epoch 隔离以及未来/移除/重入后的 SID 映射。
- 实际 SFU 明文四轨回归通过，包含初始按轨拒绝、动态授权、运行中撤销及恢复收发，另一视频持续转发。报告：仓库 `out/rtc-livekit-integration/20261029-004707-xg3d_geu/result.json`。
- 媒体加密下的完整四轨权限/接收需求/来源/静音回归通过，报告：`out/rtc-livekit-integration/20261029-005038-1b7htnzk/result.json`。H.264 加密原始视频双图复验通过：`20261029-005311-31dyn1xp/result.json`。
- 最终主构建九项 CTest 全部通过（23.74 秒），无旧 SDK 配置的 backend/pipelines/session 三项复验通过（2.17 秒）。构建日志：SDK `out/rtc-permissions-main-libs.log`、`out/rtc-permissions-main-build.log`、`out/rtc-permissions-final-main.log`。没有执行设备人工验收。

### 上层输入图与独立输入库（2026-10-29）

- `LiveKit::MediaInputs` 提供应用 I420/PCM/编码输入队列、设备采集/枚举、媒体探测与子进程支持。实现已从旧 SDK 的编译源列表移出，`probe_media` 不再经过旧 Room/SDK facade。关闭旧 SDK 的配置可直接链接此库。
- 新 `LiveKit::RTCInputs` / `nodes::MediaInputNode` 支持测试画面/音调、文件/URL、摄像头/屏幕及应用供帧。它向上层 GraphEngine 输出原始帧或编码帧，实际 RTC 编解码仍由独立 codec 节点负责。文件输入的 FFmpeg 解码位于上层。
- 输入节点等待首个绑定反馈，激活后生产；关闭时停止/唤醒读操作、退出生产者并释放独占源。重连和静音代次清理应用输入队列，源代次防止旧缓存改贴新 epoch；编码 Opus 直接透传，无 PCM 时钟，并展开 RTP 回绕。应用 PCM 保留原采样率和 10 ms 分块。
- 修复子进程已关闭输出但仍未退出时，`wait()` 持锁导致 `terminate()` 最多被阻塞十秒的问题。等待改成可终止的轮询，独立回收锁避免重复 wait；覆盖阻塞管道、EOF 后挂起、有限 PCM 尾块和损坏 I420 输入。
- 编码源等待未来采集时间时能响应代次切换和关键帧清队列；音视频 codec 拒绝静音切换前采集、切换后才送入的原始帧，防止异步源迟到的数据绕过已有队列清理。
- 视频发送示例和 SFU 双视频图已使用输入节点。无旧 SDK 的输入/真实 FFmpeg/音频 codec/视频 codec 四项测试通过（2.41 秒）；文件测试实际探测并将 44.1 kHz 单声道 WAV 转成 48 kHz 双声道 PCM。测试子进程仅用于确定性故障和退出测试，与真实 FFmpeg 检查分开。
- 最终主构建十一项 CTest 全部通过（25.58 秒），包括旧 SDK 迁移回归。实际 SFU 的 VP8 双图和 H.264 + E2EE 双图均通过输入节点、静音恢复及独立关闭：仓库 `out/rtc-livekit-integration/20261029-012058-41syv2ai/result.json`、`20261029-012058-pstrtlr0/result.json`。构建日志：SDK `out/rtc-input-boundary-build.log`、`out/rtc-input-boundary-main-build.log`。
- 摄像头/屏幕适配器已编译接入，但没有操作真实设备或桌面；麦克风、设备播放、跨轨同步和产品消费者仍待迁移。
- 输入实现移出旧 SDK 后，现有 CLI 重新编译链接成功，`lk --help` 无窗口启动通过（日志：SDK `out/rtc-input-cli-link.log`）；这仅确认过渡构建未断，CLI 的旧 Room 调用仍须迁移。

### 独立麦克风与共享音频输出（2026-10-29）

- 新 `MicrophoneSession` 位于上层输入库，独立控制原生 ADM；Room 原生 transport 继续使用 dummy audio，不引入设备、实际编解码或 APM。`MediaInputOptions::microphone` 将 48 kHz 有界 PCM 输入接入 `MediaInputNode → AudioCodecNode → RtcPublishNode`。
- 输入节点等待绑定后启动采集，退出时等待设备回调并释放源读者。房间重连只重置队列；采集提交带预期源代次，防止正在处理的旧回调跨越重连/静音边界。声道转换、重采样、10 ms 节拍和队列背压均在上层处理。
- 新 `LiveKit::RTCOutputs` 提供每轨 `AudioOutputNode` 和共享 `AudioOutputSession`。按设备 10 ms 时钟混音，各轨独立缓冲、epoch、时间戳缺口、音量及静音；溢出/过大时间跳变只重置该轨。最后一条轨道关闭时停止设备，旧句柄与同名新轨道隔离。
- 原生输入/输出回调转换器可在不打开设备的情况下测试，覆盖 ADM 的交错帧字节数、播放返回的所有声道样本总数、延迟时间、采样率/声道切换、错误静音和 out 参数初始化。虚拟驱动覆盖启动失败回收、独占输入、停止回调屏障、设备复用、多轨饱和混音、短帧和队列边界。
- `audio_source` 已改用输入节点，可通过 `--microphone` 使用实际采集；`audio_stream --playout` 可将订阅接入共享输出设备。自动化不会使用这两个设备参数。
- 主构建十二项 CTest 全部通过（26.24 秒）；关闭旧 SDK 的输入、真实 FFmpeg、共享输出和音视频 codec 五项测试通过（2.18 秒）。原生回调转换器、音频示例及旧 SDK 过渡构建均已编译验证。日志：SDK `out/rtc-audio-devices-final-main.log`、`out/rtc-audio-devices-final-independent.log`、`out/rtc-audio-devices-sdk-build.log`。
- 实际指定 SFU 的双音频图通过明文和 Opus E2EE 回归，验证 44.1 kHz 重采样、双声道、多轨混音与独立关闭。报告：仓库 `out/rtc-livekit-integration/20261029-015300-593dt4w4/result.json`、`20261029-015805-uzvd_n3c/result.json`。播放时钟由虚拟设备驱动，未打开物理麦克风或扬声器。
- 这一步没有提供 AEC/APM、NetEq/PLC 或发送端跨轨音画同步；产品调用方还未完成迁移，旧 Room API 尚未删除。

### 剩余迁移

CLI 已迁移到新 engine，详细记录见下节；以下剩余项不再包含 CLI 的旧 Room 调用。

1. 迁移 livekit-app 及剩余消费者，最终删除旧 Room API/媒体内嵌路径；当前整个重构仍在进行中。
2. 数据消息、RPC、流、DataTrack、E2EE、动态订阅需求及订阅权限已接通；原生 adapter 当前支持双 PC 及整会话重入，未实现单 PC、信令 resume 和迁房。
3. 多条独立轨道已验证；单轨 simulcast/SVC 尚未实现，当前明确拒绝。共享预算统计 SDK 管理的逻辑载荷，不包含 RTP 重传缓存、OS socket 或应用保留的数据；每轨原生层与端点层合并帧数/字节限制已实现。
4. 完善上层共享音频处理（AEC/APM）、接收乱序/PLC、参考帧依赖和跨轨同步契约。麦克风与共享播放已接入独立节点，但不等同于这些处理已完成。H.264 SPS VUI 可能被原生层改写，固定样本验证不能外推至所有码流。
5. 必须使用与本仓库扩展源码匹配的 WebRTC 库；独立完整后端构建及本机 SFU/Go 互通已通过，继续完成所有产品回归和设备人工验收。

这些自动化验证没有使用真实设备，不能视为设备人工验收或整个迁移已完成。

### CLI 消费者迁移（2026-10-29）

- `tools/cli/main.cpp` 不再包含 `livekit/room.hpp`/`livekit/livekit.hpp`；会话编排移入 `tools/cli/session.cpp`，通过 `RtcEngine` 创建每轨输入、codec、发布/订阅、输出和录制图。Token/管理接口直接链接 ServerSDK，`doctor` 只创建传输 PeerConnection 和数据通道 SDP。
- 新 `nodes::RecordingNode` 位于 `LiveKit::RTCOutputs`，直接接收拥有数据的原始媒体事件；独立有界写盘 worker、WAV 完成头、I420 stride/奇数尺寸、epoch 分段及不覆盖已有路径均有无窗口测试。原有 SDL 显示实现抽为独立 `livekit_media_display` 库，不依赖旧 Room。
- `RoomOptions::stats_interval` 和 `RoomEventKind::statistics` 提供原生 W3C 传输统计。房间适配器对每个 Peer 最多保留一个未完成统计请求，回调不进入应用。报告按目标合并，控制事件可挤掉诊断，旧 epoch 被过滤；单份报告限制 1 MiB。CLI 的 GraphEngine 统计读取实际节点计数和实际连接。
- 主构建十二项新 RTC/CLI CTest 通过（15.99 秒），包含统计准入/代次/队列压力、原生统计生命周期、录制内容与文件碰撞。
- 实际指定 SFU 的 CLI 完整回归通过：Token、管理 API、非法输入、VP8/Opus、可靠数据、录制、Unicode 路径、重复文件输入四轨发布和 EOF 退出。报告：仓库 `out/rtc-cli-integration/20261029-021712/result.json`。
- 实际图拓扑、原生 W3C RTP 统计和 SDL dummy 软件渲染回归通过：`out/rtc-cli-graphs/20261029-022329/result.json`。SDL 通过无窗口驱动呈现实际解码画面，没有操作用户桌面。
- TCP H.264 输入、按 SID 订阅、服务端静音/恢复和踢出退出测试通过：`out/stream-integration/20261029-022359`。CLI 视频编码参数现在明确接受新 backend 支持的 VP8/H.264/H.265；实际 codec 可用性仍由媒体构建决定。


### 独立 CLI 验证补充（2026-10-29）

- `out/rtc-media-contracts` 在 `LIVEKIT_BUILD_RTC=OFF` 下构建 CLI，`lk.vcxproj` 不引用 `livekit_sdk.lib`。backend、pipelines、native、outputs、CLI help 和 doctor 六项 CTest 通过（4.28 秒）。
- 此独立 CLI 对指定 Go SFU 的真实收发、逐轨图拓扑、原生 RTP 统计、WAV/Y4M 录制和 SDL dummy/software 呈现均通过。报告：仓库 `out/rtc-cli-independent/20261029-023017/result.json`。
- 主构建旧 SDK 的过渡回归也通过（9.95 秒）。这是迁移中的回归保障，不代表旧 API 已删除。


### Conference viewer 消费者迁移（2026-10-29）

- 示例的 `Session` 通过 `RtcEngine` 创建每轨输入/编码/发布和订阅/解码/预览图，全部共享同一 backend。使用应用自身 `SessionOptions` 和 UI 快照，不包含旧 Room、Participant handle 或旧 SDK facade。预览持有不可变像素；控制操作只在 supervisor 执行。
- 保留多路测试视频、独立音调、动态增删视频、房间复用、停止/取消、服务器静音/移除、逐轨录制、共享音频输出及质量面板。订阅状态来自已确认图绑定，与服务器宣告的 publication 分离。
- 原生统计新增 `publication_tracks` 映射 native track identifier 到当前 SID；图的阶段计数来自实际节点。订阅 PC 报告不会清空发布 PC 的映射，旧时间戳报告不能覆盖当前映射。
- `TrackDescriptor::follow_subscriber_demand` 处理服务器单层视频 aggregate ON/OFF；运输层关闭准入并使旧队列失效，上层输入暂停生产、codec 清理待处理帧，恢复请求关键帧。文件/URL 输入不能使用此暂停策略。UDP/TCP 直连选择下沉为 signaling 网络配置，独立于旧 Room。
- 示例和 SDL 像素测试可在 `LIVEKIT_BUILD_RTC=OFF` 下构建。十项 CTest 通过（9.37 秒），包含 native 发送队列 OFF/ON 隔离、generated input 暂停恢复、独立输出、输入、真实 FFmpeg、CLI 及 SDL dummy/software 渲染。
- 真实指定 Go SFU 的多流/动态增删/逐轨质量/录制/重复会话/取消/服务器移除回归，以及 UDP、TCP 两种单层 aggregate demand 回归通过：仓库 `out/rtc-conference-independent/20261029-025141/result.json`。两个需求控制报告验证初始和动态视频都暂停，仍有一个观看者时继续发送，恢复保持原 SID；没有打开物理设备。
- 后续补上本地 roster 的时序保护：AddTrack ACK 与 participant version 不共享序列，延迟的自身成员快照不能抹掉已确认 publication；显式退订发布的 SID 留下有界 tombstone，防止旧快照复活。对应确定性单测通过，进一步真实 SFU 回归进行中。

### 会话重入与投屏产品验证（2026-10-29）

- 相同 identity 快速重入时，roster 替换旧 SID 并清理旧订阅与会话服务；旧 SID 的延迟离开/更新不能覆盖新成员。主配置 15 项 CTest 通过（29.45 秒），独立配置 session 与 demo_render 通过。
- Go SFU 的 CLI、会议示例、多流、动态增删、重复会话和 UDP/TCP demand 回归通过：`out/rtc-conference-identity/20261029-032225/result.json`；另一次重复会话回归通过：`out/rtc-conference-identity-repeat/20261029-032441/result.json`。
- 投屏产品使用独立 `RtcEngine`，输入、预览、编码和发布均在上层 GraphEngine。停止由主管线程完成，无旧 Room/facade 依赖；独立配置 `livekit-app/out/screen-cast-rtc-qt6` 禁用旧 SDK，Qt 离屏/会话/采集契约五项 CTest 通过（8.05 秒）。
- 合成投屏到无头 Chromium 的真实业务/媒体集成通过，覆盖两会话隔离、实际解码、本地预览、接收方停止、发送方崩溃/停止、取消和服务失联。证据：`out/screen-cast-rtc-integration/evidence.json`。未采集物理屏幕或操作用户桌面。
- 会议产品迁移及服务端主动撤销轨道的完整清理正在实现、验证。上述成功报告早于此后续改动，不能替代后续回归。

### 会议产品迁移及服务器主动移除轨道（2026-10-29）

- `livekit-app/apps/conference/desktop` 移除 Room/Participant facade，采用产品 `SessionOptions`、UI 成员模型及独立 `session_runtime.cpp`。主管线程持有 RtcEngine；输入、编解码、本地预览、订阅显示、录制和共享音频输出按轨连接。可靠中文聊天通过共享 backend 的 RoomServices；发言状态、静音、来源和 TokenProvider 保留。
- 产品独立配置 `livekit-app/out/conference-rtc-qt6` 禁用旧 SDK。完整 GUI、三类集成客户端、聊天与 Qt 离屏测试编译成功，不链接 `livekit_sdk.lib`。三个 CTest 通过（3.33 秒）；程序内截图位于 `out/conference-rtc-qt-screenshots`。
- Go SFU 多流测试通过：两发送者、多视频、本地预览、逐轨静音、动态增删、其他轨继续、WAV/Y4M 录制、重入、取消、非法 Token 和服务器移除。报告：`out/conference-rtc-product/20261029-033931/result.json`。可靠聊天的双向 Unicode/多行、房间隔离和重入验证：`out/conference-chat/2026-10-28T19-37-52-959843Z/evidence.json`。
- C++ SFU + 当前 Go 业务服务的真实会议集成通过：`out/conference-rtc-business/20261029-034017/result.json`。覆盖等待室批准、实际视频及非静音音频解码、聊天、主持人静音、共享权限授予/撤销、撤销后摄像头和音频继续、重入、踢人/会议结束及旧 Token 拒绝。原有业务二进制不支持 conference 参数，测试改为从当前源码另行构建 `out/conference-rtc-business-api.exe`，未覆盖原产物。
- 修复 native `TrackUnpublishedResponse` 只更新 roster 的问题：立即停止发送并移除对应 sender，清理映射/发布意图。迟到 AddTrack ACK 不可复活已退役 SID。正常移除发出 `RoomEventKind::track_removed`（携带 descriptor），engine 关闭对应图，保留共享房间与其他流；CLI 和两个会议调用方处理此通知。正常订阅移除也与 codec/transport 失败分开。
- backend/pipelines/session 三项确定性检查通过（2.65 秒），包含服务器移除通知、图关闭、共享连接隔离；roster 新增满容量同 identity 替换及迟到离开边界测试。独立其余 10 项自动化通过；此前未构建的 `rtc_services` 目标补编译后通过（9.52 秒），共 14 项独立检查完成。
- 本轮没有物理摄像头/麦克风/扬声器人工验收。远控、直播工作台、旧 Room 清理及前述音频处理/同步能力仍待完成，整个目标尚未完成。

### 远控产品迁移（2026-10-29）

- 原生被控端和控制端均移除旧 Room/Participant facade，采用产品内部 `RtcConnection`。一个媒体主管线程持有一个 backend/engine；屏幕发送由输入→VP8 编码→发布图完成，接收由独立订阅→解码→有界最新帧图完成。UI 不持有 Room，也不参与 engine 生命周期调用。
- 控制消息经同一 backend 的 `RoomServices`；每轮接收有上限，保留授权身份、可靠消息、包大小及协议状态验证。只订阅授权身份的公告轨道，展示前再次核验归属；绑定已观察到的 participant SID，离开或被同身份新连接替换即终止控制，不自动恢复授权。
- 退出和异常先释放输入；授权与停止串行，停止后不可再次授权。正常 peer stop/leave 以正常结束呈现，异常仍报告错误；加入失败信息过滤 Token。等待加入/发布时通过停止标记取消，图在主管线程关闭。
- `LiveKit::RemoteControl` 独立提供协议和平台输入适配器，不再从旧 SDK 编译；`DataReceiveParams` 移到中立 data_messages 头。独立产品配置 `livekit-app/out/remote-control-rtc-qt6` 使用 `LIVEKIT_BUILD_RTC=OFF`，GUI/fixture 不链接 `livekit_sdk.lib`。移除没有源码/资源使用的 Qt Svg 必需依赖。
- GUI、两个 fixture 和全部产品测试编译完成；5 项 CTest 通过（5.12 秒）：生命周期取消/复用、SDK 协议、控制端协议/I420、Qt 离屏常规/HiDPI。独立控制库的主 SDK 测试也通过（0.20 秒）。
- 最新 C++ SFU 原生双端集成通过：`out/remote-control-rtc-native-final/evidence.json`，覆盖两会话隔离、视频解码、Unicode 文本确认、正常停止成功退出、控制进程退出及业务失联时释放按键。
- 最新 C++ SFU 无头 Chromium 集成通过：`out/remote-control-rtc-browser-final/evidence.json`，覆盖真实视频、失焦释放、中文/emoji/多行文本、长度/重复提交限制、关闭文本能力、延迟握手、正常退出、心跳和业务服务失联。原生输入采用记录 sink，视频完全合成，没有采集或注入用户桌面。Go SFU 同套无头浏览器互通验证也通过：`out/remote-control-rtc-browser-go/evidence.json`。
- 直播工作台及旧 Room API/内嵌媒体路径仍待迁移和清理。整个重构目标保持未完成。

### Studio 迁移与旧 facade 清理（2026-10-29）

- Studio 库独立链接 MediaInputs、Player 和 GraphEngine；产品使用每房间一个媒体主管线程，节目/互动各自的音频与视频发布图、嘉宾每轨订阅图，有界不可变输出队列。正常房间/发布移除不作为 codec 故障；业务线程负责重新连接和状态，不跨线程操作 engine。
- Live-studio GUI、fixture 和离屏测试在旧 SDK 关闭配置下编译成功，8 项 CTest 通过（18.78 秒）。首次原生浏览器联调在嘉宾画面处超时；增加逐轨诊断后两次完整联调通过，不以首次失败作为成功证据。
- 最新 C++ SFU + 原生双主播 + 无头 Chromium 报告：livekit-app/out/live-studio/integration-1793218437147/result.json。覆盖实际节目视频/音频、嘉宾视频和 PCM 分别解码、节目合成、双主播 PK、撤销权限、业务消息、停止与 MP4 收尾。使用 WARP 和合成输入，不访问物理设备。上一轮通过报告：integration-1793218261137/result.json。
- 删除旧 Room/Participant/TrackPublication 公共 facade、旧 AudioFrameStream/VideoFrameStream 和内嵌 RTC 媒体图、codec 工厂、MediaPump。AudioFrame 移到中立 audio_frame.hpp；RTP/编码文件工具移到上层 capture 库。CMake 不再定义 LiveKit::SDK 或 LIVEKIT_BUILD_RTC。
- 输入、消息协议、Token/region 策略、控制协议、FFmpeg codec 等仍有价值的契约测试改为直接链接对应独立库。旧 API 集成夹具与文档的迁移审计尚未完成，整个目标保持进行中。
- 远控独立配置的控制库测试补充通过（0.19 秒），日志：SDK out/rtc-remote-independent-build.log。


### 最终迁移审计与验证（2026-10-29）

- 所有当前 C++ 调用方及 CMake 已不引用已删除的 Room/Participant/帧流头文件或 LiveKit::SDK。公共头本地 include 审计仅需 CMake 生成的 version.hpp；当前用法集中在 README 和 rtc-encoded-api.md，旧主题文档显式标为历史接口。
- 旧 sdk/reconnect/rejoin/room_moved C++ 夹具及依附它们的 helper 已删除。替代测试和未迁移能力逐项记录在 tests/README.md；保留的 Python 公共工具移入 sfu_test_support.py。Go runner 仅保留当前 backend 支持的 plain/encrypted/encoded/media-e2ee 模式，media_graph runner 删除旧 --sdk-client 入口。
- 异步 join 保留 SDKError 的 domain/code/retryable/backend_reason。初次媒体建立失败可在总期限内选择可信备用端点，独立于已连接会话的 reconnect 开关；失败端点的初始成员/轨道事件不对外发布。服务端强制 relay 与显式 UDP/TCP 直连冲突会返回 unsupported。
- 区域测试通过：out/region-integration/20261029-045045/result.json。覆盖可信重定向、HTTP 重定向拒绝、初始信令/ICE 失败切换、终止错误不重试、总期限、取消、relay 冲突；forbiddenRequests=0。
- 新完整 rejoin 夹具通过：out/rtc-rejoin-integration/20261029-044902/result.json。真实 SFU 上两次中断、三次完整加入，两个视频流恢复且不串流；稳定本地 ID、更新 SID、静音意图、旧 epoch 拒绝、已移除轨道不复活及终止移除均通过。不将此结果表述为 signaling resume。
- 多摄像头/多观看者、三方六向音频、UDP/TCP 视频、订阅需求和直连拒绝六组测试全部通过：out/rtc-uav-migration/20261029-043719/result.json。
- 最新四轨 H264/Opus 媒体加密回归通过：out/rtc-livekit-integration/20261029-045156-fbnn4py3/result.json；包含独立关闭、重订阅、取消、会话状态、成员控制、静音、来源、订阅需求和发布权限。
- 最新 Go SDK 互通通过：out/extensions-media-e2ee/20261029-045240-7za28gem/result.json；100,000 字节文本、300,000 字节文件、40,000 字节 DataTrack、加密向量和 H264 媒体，接收码流经 FFmpeg 解码检查。
- 关闭 native/server/media 的 backend+GraphEngine 配置编译成功，2 项 CTest 通过（3.36 秒）；关闭 GraphEngine/media/server/player 的 native 配置编译成功，6 项 CTest 通过（13.83 秒）。新传输层不依赖上层实际 codec 或设备节点。
- PCM 输入夹具在并行编译负载下触发了合法的实时跳帧/欠载静音，原先固定“四个连续时隙”的断言不成立。夹具改为持续供帧，核对四个实际数据块、完整样本、静音内容及按 capture time 的 48 kHz 时间戳；连续五次通过。生产逻辑未因这次测试调整而改变。

所有结果属于自动化检查；没有执行物理摄像头、麦克风、扬声器或桌面操作人工验收。代码未提交。

最终完整 SDK 配置的 21 项 CTest 全部通过（26.25 秒），四个 RTC 产品聚合配置编译成功且 21 项 CTest 全部通过（23.72 秒）。日志分别保存为 SDK out/rtc-migration-final-ctest.log 和 livekit-app/out/rtc-products-final-ctest.log。当前源码、私有 include 路径及 Python runner 语法检查通过。

### 配置式媒体引擎与共享采集（2026-10-29，后续调整）

- 新增可选 `LiveKit::RTCMediaEngine` 和 `rtc/engine.hpp`（配置类型在 `rtc/media_config.hpp`）。Session 提交 MediaPublishSpec / MediaSubscribeSpec，读取 MediaTrack 快照和拥有数据的像素/PCM 回调；采集、编解码、预览、播放、录制节点及反馈连线由媒体引擎创建。RtcEngine 保留纯编码和高级图接口，RoomEngine 保留房间、信令及传输职责。
- 会议示例、会议产品、CLI、投屏、远控、Studio 及音视频公共示例迁移到配置式入口；常规 Session 不再创建 GraphNode。PCM、codec 配置及快照移至中立 media_types.hpp。room_client 保留高级图示例。
- SharedMediaInput 用独立 GraphEngine 源图扇出到每路有界队列，可跨房间共享原始采集。第一路激活时开始，最后一路结束时停止；各发布独立编码、反馈和关闭。外部应用源只借用读者，取消、失败与关闭释放借用，不永久关闭源。共享输入要求相同视频尺寸/音频声道数，不支持文件/URL、编码源或逐分支缩放。
- 新增真实 VP8/Opus 编解码回环、多流独立关闭、回调停止屏障、取消/启动失败后源复用、跨房间共享视频、共享 44.1 kHz 双声道 PCM 和失败分支隔离测试。首次模拟传输测试出现超时，补齐模拟关键帧反馈后连续五次通过（9.89 秒）；未把首次失败算作成功。
- 最终 SDK 构建及 22 项 CTest 全部通过（27.13 秒），四产品聚合构建及 21 项 CTest 全部通过（25.98 秒）。日志：根目录 out/media-engine-sdk-build.log、out/media-engine-sdk-ctest.log、out/media-engine-products-build.log、out/media-engine-products-ctest.log。
- 关闭 native/media/server 的 backend+GraphEngine 配置编译及 2 项 CTest 通过（1.76 秒）；关闭 GraphEngine/media/server/player 的 native 配置编译及 6 项 CTest 通过（12.05 秒）。新增层未进入纯传输依赖。
- Go SFU 的 CLI 图拓扑、编码收发、无窗口视频呈现、WAV/Y4M 录制、会议动态多流和 UDP/TCP demand 通过：根目录 out/rtc-media-engine-integration/20261029-100454/result.json。迁移后的音频示例实际 Opus 收发通过：out/rtc-livekit-integration/20261029-100520-bj7sm_fc/result.json。
- 最终产品二进制的 C++ SFU 原生远控集成通过，覆盖双会话、实际视频、Unicode 文本、正常停止、控制进程退出与业务失联释放：out/rtc-media-engine-remote/evidence.json。
- 最终 Studio fixture 的 C++ SFU + 无头 Chromium 集成通过，覆盖节目视频/音频、嘉宾解码及合成、双主播 PK、权限撤销、业务控制和录制完成：livekit-app/out/live-studio/integration-1793239942574/result.json。runner 新增 STUDIO_NATIVE_FIXTURE 路径选择，避免误用旧构建。
- 当前接口和所有权规则见 [媒体引擎 API](rtc-media-engine.md)。未新增 AEC/APM、NetEq/PLC、跨轨音画同步或多编码层能力。以上均为自动化检查，没有物理设备人工验收，也未操作用户桌面。代码未提交。

### RoomEngine 命名与后端目录整理（2026-10-29）

- 公共房间类型及头文件改为 `RoomEngine` / `livekit/rtc/room_engine.hpp`，内部状态改为 `RoomEngineState`；工厂、引擎、节点、测试和文档同步更新，不保留旧类型别名。CMake 模块目标与开关保持不变。
- 公共控制器、端点、预算和适配接口留在 `src/backends/rtc/`；LiveKit 信令、传输适配、RPC、流、数据消息、加密及重连策略移到 `rtc/livekit/`。PeerConnection、编码帧收发和传输运行时移到 `backends/webrtc/`；依赖 LiveKit 协议的帧加密适配器移回 `rtc/livekit/`。
- 内部消息和加密头分别命名为 `data_message_wire.hpp` 与 `packet_encryption.hpp`，避免 MSVC 沿包含栈搜索时遮蔽公共 `livekit/data_messages.hpp` / `livekit/encryption.hpp`。
- `rtc/mediasoup/` 仅记录扩展边界，没有实现或构建目标。现有 `RoomOptions` 与 `RoomServices` 语义保持不变，后续接入需要拆分后端连接配置并明确可选服务能力。
- 自动化验证：`out/rtc-backend-contracts` 编译及 2 项 CTest 通过；`out/rtc-native-contracts` 编译及 6 项 CTest 通过；`out/rtc-media-contracts` 中已重新编译的 `rtc_pipelines`、`rtc_inputs`、`rtc_input_ffmpeg`、`rtc_audio_nodes`、`rtc_video_nodes`、`sdk`、`client_extensions` 共 7 项定向测试通过。40 个移动文件的非包含内容核对、内部路径解析及 diff 空白检查通过。
- 本次完整媒体构建未通过：当前提交中的 `src/media_nodes/playout/audio_output.cpp`、`native_audio_output.cpp`、`audio_output_driver.hpp`、`audio_output_transport.hpp` 均为空文件，造成音频输出测试编译失败及媒体引擎消费者链接失败。这 4 个文件未在本次修改；完整构建日志位于聚合仓库 `out/room-session-media-build.log`。以上历史全量通过记录不代表本次全量验证结果。
- 未执行实际设备人工验收或操作用户桌面；未提交代码。

### 音频播放构建阻塞修复（2026-10-29）

- 确认格式化提交 `188df04` 清空了上节列出的 4 个音频播放文件；从前一提交 `5dd88ef` 恢复共享 PCM 混音、GraphEngine 输出节点、设备驱动及 ADM 回调适配，补齐直接使用的标准库头文件，并使用 clang-format 20.1.8 格式化。
- 在隔离夹具中复现 `**/out/**` 对 `playout/` 的错误匹配：格式化进程成功退出但输出为空。新增 `src/media_nodes/playout/.clang-format-ignore` 覆盖父级忽略规则；验证 4 个源码实际参与格式化且输出非空，生成代码和 WebRTC 上游源码仍被排除。仓库代码规范补充了非空输出校验与禁止直接重定向回输入文件的要求。
- 完整 `out/rtc-media-contracts` Release 构建成功，包含 CLI、会议示例、音视频示例及音频图集成目标；全部 22 项 CTest 通过（28.45 秒），包含共享音频输出、媒体引擎、编解码及无窗口渲染。上节记录的完整媒体构建阻塞已解除。
- 日志位于聚合仓库 `out/audio-playout-repair-full-build.log` 和 `out/audio-playout-repair-ctest.log`。格式检查及 diff 空白检查通过；结果属于自动化验证，未进行实际设备人工验收，未操作用户桌面，也未提交代码。


## 统一 RtcEngine 入口

- RtcEngine 组合 RoomEngine、内部 RtcPipelineEngine 和可选 RtcMediaEngine；媒体模块不再持有顶层引擎。
- RoomSession 更名并演进为 RoomEngine；公开媒体引擎头与旧工厂移除，应用、CLI、示例迁移到统一工厂。
- room() 返回受控 RoomView，房间事件由 next_event() 单一消费；轨道关闭与退房经过顶层屏障。
- leave 支持随后重新 join，使用新传输适配器及递增 epoch；close 永久终止引擎。
- 保留纯编码构建，媒体模块显式装配；内部每轨流水线继续使用 GraphEngine。

验证：Windows 编译、24 项自动化测试和隔离 SFU 集成通过，见 [统一入口验证记录](rtc-engine-validation-20261029.md)。
