# Go SDK 核心 API 适配

参考：同级 `server-sdk-go/v2`，revision `743ab2d6b12ff24c2b2f9a0cc208c7afe8602571`，Go module 使用 `livekit/protocol v1.53.0`。C++ 实现参考其对象模型、权限、wire 字段和调用语义，媒体底层为 `../webrtc` M137，不运行 Go/Pion。

## 客户端对应关系

| Go SDK | C++ SDK | 行为 |
| --- | --- | --- |
| `NewRoom(callback)` | `Room(RoomCallback)` | 每实例独立会话、状态和回调 |
| `ConnectToRoomWithToken` / `JoinWithToken` | `connect_to_room_with_token` / `join_with_token` | 等待连接成功返回；无需重复指定 Token 中的房间名 |
| `ConnectToRoom` / `Join` | `connect_to_room` / `join` + `ConnectInfo` | key/secret 签名，携带 identity/name/metadata/attributes |
| `Disconnect` / `DisconnectReason` | `disconnect` / `disconnect_reason` | 保留原始协议原因；可在应用层区分踢人、房间删除等 |
| `GetRemoteParticipants` / `GetParticipantByIdentity` / `GetParticipantBySID` | `remote_participants` / `participant_by_identity` / `participant_by_sid` | 包括入会时已有参与者；线程安全快照 |
| `LocalParticipant.PublishTrack` | `local_participant().publish_track` | 动态增加原生 transceiver，等待 TrackPublished 确认后返回 publication |
| `UnpublishTrack` | `unpublish_track` | 移除 sender、停止对应源、重新协商；可继续发布新轨道 |
| `PublishData` / `WithDataPublishReliable` / `WithDataPublishDestination` | `publish_data` + `DataPublishOptions` | 默认 lossy；可靠通道、topic、定向 identity、二进制 payload |
| `SetName` / `SetMetadata` / `SetAttributes` | `set_name` / `set_metadata` / `set_attributes` | 需 Token `canUpdateOwnMetadata`；属性空值表示删除 |
| `RemoteTrackPublication.SetSubscribed` | `set_subscribed` | 发送订阅变更，异步订阅状态由服务器协商确定 |
| `SetEnabled` / `SetVideoDimensions` | `set_enabled` / `set_video_dimensions` | 修改远端流设置并保留已有设置 |
| `LocalTrackPublication.SetMuted` | `set_muted` | 同时设置本地 track enabled 和服务器 mute 信令 |
| `RoomCallback` / `ParticipantCallback` | `RoomCallback` | 参与者、publication、订阅、静音、数据、metadata/属性、room metadata/录制、speakers、连接质量 |
| Connect options | `RoomOptions` | 自动订阅/指定轨道、relay-only、禁用 TURN、超时；其余媒体选项沿用 CLI |

C++ 不暴露 Pion 或 libwebrtc 类型。`ParticipantInfo` 和 `TrackInfo` 是值对象，`TrackPublication` 是带会话代数的轻量句柄。断开后尚未完成的请求以异常结束，旧代数句柄不能操作新会话。`publish_track` 的输入是 `MediaSpec`（生成图案/音调、FFmpeg 文件/网络源或应用 I420 源）；默认麦克风通过 `RoomOptions.microphone` 启用。

`on_track_subscribed` 暴露轨道状态；音视频由 native sink 自动接收并解码，通过录制/播放消费。尚未提供 Go `TrackRemote.ReadRTP`、`LocalSampleTrack` 对应的原始 RTP/编码样本扩展。工作区同期新增的视频帧回调和会议 Demo 属于独立开发，本轮 Go 适配验收不包含 Demo。

客户端补齐增量已增加 `RoomOptions.video_frame_stream` 和 `audio_frame_stream`：应用从有界队列读取不可变 I420 帧或逐轨 PCM16/float32 块，慢消费者不阻塞信令；支持关闭、取消、容量统计及重连代次。具体接口与边界见 [有界视频帧读取](video-stream.md)、[逐轨 PCM 读取](audio-stream.md)。这些不等价于 RTP 读取。

`MediaSpec.video_source` 可绑定 `I420VideoSource`，提交具有独立像素所有权的 CPU I420 视频，支持有界背压、过期帧清理和已确认源的完整 rejoin。首版使用提交时的本地单调时钟，固定尺寸且要求已转正；视频外部采集时钟、RGBA 转换、编码帧和 RTP 接入仍待提供。具体边界见 [应用 I420 输入](video-source.md)，验证结果见 [实施记录](client-parity-progress.md)。

`MediaSpec.audio_source` 可绑定独立 `PcmAudioSource`，支持 PCM16/float32、8/16/24/32/44.1/48/96 kHz、单/双声道，以 10 ms 整数倍输入并逐轨重采样至 48 kHz/Opus。可提供 steady_clock 采集时间，缺口/欠载补静音、过载返回背压，完整 rejoin 清空旧时间线；详见 [应用 PCM 输入](audio-source.md)。Go PCM 接收工具在其源码中仍声明不支持立体声，不能将 C++ 立体声测试当作 Go 立体声互通结果。原始 PCM/编码样本互操作验收继续待完成。

`set_name` / `set_metadata` 遵守 Go 的 proto3 字符串字段语义，空字符串表示不变，不能用来清空对应字段；`set_attributes` 可以通过空值删除键。默认 future 完成表示更新信令已经发送；显式选择 `RoomOptions.signal_protocol = 15` 和 `OperationOptions.wait_for_ack = true` 后可等待服务器 RequestResponse，权限拒绝返回类型化错误。默认 protocol 12 不支持成功确认，该选项会在发送前被拒绝。操作级取消/超时及结果未知的处理见 [操作契约](reconnection.md#操作取消超时与确认)。

## 服务端对应关系

`LiveKitAPI` 统一提供 `room()`、`egress()`、`ingress()`、`sip()`、`agent_dispatch()`、`connector()`。所有方法接受 `Json` 请求和可选 `RequestOptions`，返回 `Json`，字段和枚举遵守 LiveKit protobuf JSON，例如 `roomName`、`destinationIdentities`、`"RELIABLE"`，bytes 使用标准 Base64。Room 常用方法另外保留字符串参数便捷重载。

| 客户端 | 已适配 RPC（C++ 使用 snake_case） | 每次签名权限 |
| --- | --- | --- |
| RoomService | CreateRoom, ListRooms, DeleteRoom, ListParticipants, GetParticipant, RemoveParticipant, ForwardParticipant, MoveParticipant, MutePublishedTrack, UpdateParticipant, UpdateSubscriptions, UpdateRoomMetadata, SendData | 创建/删除 roomCreate；列表 roomList；其余 roomAdmin + room；转移/转发加 destinationRoom |
| Egress | StartEgress, StartRoomCompositeEgress, StartParticipantEgress, StartTrackCompositeEgress, StartTrackEgress, StartWebEgress, UpdateLayout, UpdateStream, ListEgress, StopEgress | video.roomRecord |
| Ingress | CreateIngress, UpdateIngress, ListIngress, DeleteIngress | video.ingressAdmin |
| SIP | Create/Update SIPInboundTrunk 和 SIPOutboundTrunk；ListSIPTrunk / Inbound / Outbound；DeleteSIPTrunk；Create/Update/List/Delete SIPDispatchRule；CreateSIPParticipant, TransferSIPParticipant | 管理为 sip.admin；呼叫为 sip.call；转接再加 roomAdmin + room |
| AgentDispatch | CreateDispatch, DeleteDispatch, ListDispatch | roomAdmin + room |
| Connector | ConnectTwilioCall, DialWhatsAppCall, AcceptWhatsAppCall, ConnectWhatsAppCall, DisconnectWhatsAppCall | roomCreate；前三个方法还带 room |

额外提供 `get_dispatch(id, room)`：使用 ListDispatch 查询，未找到时返回 JSON null。`SendData` 自动生成 16 字节 nonce。SIP 等待接听和转接显式填入默认 `ringingTimeout="30s"`（调用者未传时），HTTP timeout 至少为振铃时间加 2 秒，同时保留显式取消。

`AccessToken` 对应 Go auth token builder：identity、name、metadata、attributes、kind、有效期、video/sip/agent grants；HS256 签名。服务请求生成 10 分钟 Token 和 UUID v4 `X-Livekit-Request-Id`。预签发 Token 原样发送，不重写授权。

凭证解析对照 `livekitapi.go`：

1. 显式 URL 优先，缺省读取 `LIVEKIT_URL`。
2. 仅当所有显式凭证均为空时，读取 `LIVEKIT_TOKEN`。
3. 仍无凭证时才读取 `LIVEKIT_API_KEY` 与 `LIVEKIT_API_SECRET`。
4. 显式 Token 优先于同时给出的 key/secret；显式不完整 key/secret 报错，不与环境混合。

`APIError.status()`、`code()`、`what()`、`metadata()` 保留 Twirp 错误信息。`RequestOptions.stop` 支持预取消和执行中取消；每次请求独立 HTTP client，无自动重试，避免重复执行有副作用的请求。与 Go 当前实现不同，未提供跨服务共享连接池、多 URL 故障转移或 SIP/Ingress 扩展查询 helper 的完整集合。

## 线程与兼容边界

`join*` 在 SDK 拥有的后台线程运行会话，`run` 在调用线程运行。回调按会话串行分发，不持有内部状态锁；不能在回调中等待该会话尚未完成的 future。应用可从其他线程查询状态和提交命令。join/disconnect/wait 生命周期操作由应用串行调用。

保留原有 CLI 和阻塞 run 入口；CLI 自己处理 SIGINT/JSON 输出，业务仅调用公开 SDK。SDK 中服务器 Leave 结束会话并保存原因；CLI 对非用户请求的服务器断开返回错误状态，以保持迁移前的命令行行为。

2026-10-27 增量已支持信令 resume、publisher ICE restart，以及无法恢复时的完整 rejoin。`RoomOptions.reconnect` 控制共用的退避、重试次数和时间预算；`on_reconnecting/on_reconnected` 报告恢复模式。resume 保留原 PeerConnection 和句柄；完整 rejoin 重新发布已确认的源，旧句柄失效，通过稳定 `TrackInfo.client_id` 关联新 publication。未确认命令不盲目重放；有限文件源需显式允许重启。已接入异步 TokenProvider、可信区域重定向、RoomMoved 和部分 ClientConfiguration；真实 Cloud 跨区与服务端无缝迁移仍未验收。RPC、文本/字节流、转写、DataTrack、simulcast/SVC、E2EE 仍待实现。具体语义见 [连接恢复](reconnection.md)，证据见 [客户端补齐进度](client-parity-progress.md)。无窗口多 Room 可并发；SDL 窗口仍要求主线程。

## 验证层次

- CTest：原生 WebRTC、SDK 生命周期/回调、媒体和 CLI 基础测试。
- `tests/sdk_integration.py`：真实 LiveKit Server，同一进程多 Room；动态发布/取消/再发布、订阅切换、静音、binary/reliable/lossy/targeted data、metadata/属性、room metadata、只订阅 Token、取消与旧句柄失效；验证实际 WAV/Y4M。
- `tests/api_contract.py`：49 个 RPC 的 HTTP 路径、方法权限、独立 HMAC 验签、JWT 字段、UUID、nonce、预签发 Token 和显式凭证优先级、Twirp 错误、取消、SIP 默认振铃参数。模拟 HTTP 服务，不代表部署了外部服务。
- `tests/integration.py` / `tests/stream_integration.py`：迁移后的 CLI，真实服务端和 FFmpeg 输入回归。

实际结果和环境见上级 [VALIDATION.md](../VALIDATION.md)。
