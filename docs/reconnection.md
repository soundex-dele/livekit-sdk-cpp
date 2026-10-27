# 连接恢复

当前实现使用同一 `ReconnectOptions` 预算管理信令恢复和完整重新入会，默认启用。首次信令连接另支持可信候选端点切换，使用 `connect_timeout` 总期限；不将首次切换报告为已建立会话的重连。

```cpp
livekit::sdk::RoomOptions options;
options.reconnect.enabled = true;
options.reconnect.max_attempts = 6;
options.reconnect.resume_attempts = 2;
options.reconnect.timeout = std::chrono::seconds(30);

livekit::sdk::RoomCallback callbacks;
callbacks.on_reconnecting = [](const livekit::sdk::ReconnectInfo& info) {
  // 状态已经是 reconnecting。停止依赖当前连接的业务操作；远控必须释放输入。
};
callbacks.on_reconnected = [](const livekit::sdk::ReconnectInfo& info) {
  // resume：原会话句柄仍有效。
  // rejoin：重新查询 Participant / TrackPublication 并绑定新句柄。
  // 不要在回调内等待同一个 Room 的 future。
};
```

## 两种恢复路径

| 路径 | 行为 | 句柄 |
|---|---|---|
| resume | 保留原 PeerConnection，带原 participant SID 恢复信令，发送 SyncState，更新 ICE 配置并重启已协商的发布端 ICE | 保持有效，原 SID 不变 |
| rejoin | 销毁旧传输，重新入会，重新发布已确认的媒体源，并恢复可以明确匹配的远端设置 | 旧 Participant/Publication 全部失效，新服务器 SID 重新分配 |

信令断开、心跳超时、已建立的媒体传输失败会先尝试恢复稳定会话。服务端无法恢复原会话、明确要求完整重连，或发布/SDP 处于不确定状态时，转为完整重新入会。服务端拒绝授权、踢人、删除房间、重复 identity 替换和主动离开不自动重试；这些原因优先于响应中矛盾的重连指令。

两种路径共用总尝试数和单调时钟截止时间，不会因切换路径重新获得一份超时预算。只有媒体传输重新就绪后才发 `on_reconnected` 并重置预算；恢复过程中仍可取消。两种恢复均不重复发初次 `on_connected`，最终结束时只发一次 `on_disconnected`。

服务端 `ClientConfiguration.resume_connection = DISABLED` 会跳过 resume 并直接重建连接；`force_relay = ENABLED` 在初次连接和恢复时使用原生 ICE relay 策略。用户的 `relay_only` 始终优先保持，`disable_turn` 与强制中继冲突时明确报错。恢复响应省略整个配置时保留之前的策略。codec/hardware 配置仍在后续范围内。

## 发布与状态恢复

本地 `TrackInfo.client_id` 是 SDK 源 ID，在同一个 Room 生命周期内跨 rejoin 保持稳定。它可用于将新 publication 重新关联到应用的摄像头、屏幕或文件源；不要把旧服务器 SID 当作稳定源 ID。

- 已确认发布的源保留名称、stream、source 和静音状态。被取消发布的轨道不恢复。
- 未确认的发布、未完成的命令被明确结束，不盲目重放。普通数据消息没有离线队列；CLI 的一次性初始消息成功发送后不在 rejoin 时重复发送。
- 原会话已观察到的名称、metadata、attributes 在重新入会后按差异请求恢复，最终以服务器事件为准。
- 远端订阅、禁用和尺寸请求先按原 track SID 关联；SID 变化时按参与者 identity、轨道名称、kind 和 source 唯一匹配。存在多个候选时明确失败，不任意选择轨道。
- 重连后录制进入 `record/rejoin-<generation>/`，避免覆盖旧会话文件。

## 文件和外部输入

`MediaSpec.reconnect` 明确指定完整重新入会后如何处理媒体源：

| 策略 | 行为 |
|---|---|
| automatic（默认） | 生成源、设备源、loop 源及未关闭的应用 I420/PCM 源可重启；有限文件和普通网络输入不自动重播 |
| restart | 显式允许重新打开源；文件从头开始，不承诺接着旧位置播放 |
| fail | 完整重新入会时报告该源不可恢复，即使它是生成源或设备源 |

信令 resume 不重新打开媒体源，以上策略仅影响完整 rejoin。应用 I420/PCM 源重建时清除旧队列并递增输入代次，重新协商前返回背压，之后继续接收新帧；PCM 同时重置采样时间线和重采样滤波器。不得在重连期间将同一源重新绑定到另一个 Room。关闭的源应显式取消发布，不能用于完整 rejoin。详见 [应用 I420 输入](video-source.md)、[应用 PCM 输入](audio-source.md)。编码帧源的恢复契约将在对应接口交付时扩展。

## 业务 TokenProvider

可在 `RoomOptions.token_provider` 中接入自己的业务服务。`join_with_token(url, {}, options)` 在初始 Token 为空时调用 provider；已有 Token（包括服务端刷新 Token）优先使用。如果握手返回 HTTP 401，则在当前连接尝试内刷新并重试一次；403 直接报告权限错误。provider 返回的新 Token 仍被拒绝时不循环刷新。

```cpp
options.token_provider = [auth_service](livekit::sdk::TokenRequest request,
                                      livekit::sdk::TokenCompletion done) {
  // 应用异步服务：保存 request/done 的值，并马上返回。
  // 按 request.deadline 和 request.stop 取消网络请求。
  auth_service->fetch_token(request, std::move(done));
};
options.token_timeout = std::chrono::seconds(5);
```

provider 启动函数运行在会话线程，必须及时返回，不能在其中等待 Room 操作。SDK 不会创建无法取消的后台线程替应用运行阻塞函数。完成回调可在任何线程或启动函数内调用；仅首个有效结果被接收，超时/取消后的结果被忽略。`TokenFailure::unavailable` 表示可重试的业务服务故障，`denied` 表示拒绝授权；错误文本不携带 provider 的原始异常。密钥和 Token 不进入事件日志。

等待上限为 `token_timeout`、当前连接 deadline、恢复总预算和原逻辑会话期限中的最早值。取消/完成时 SDK 通知 provider 的 stop_token；异步任务由应用管理，不得捕获失效引用。SDK 无法强行中断不遵守契约的用户启动函数或阻塞 stop 回调。

## 房间迁移

`RoomCallback.on_room_moved` 接收 `RoomMovedInfo`，包含迁移前后的 RoomInfo。回调执行前，SDK 原子更新房间 SID/name/metadata/recording、本地参与者及目的房间参与者快照。旧房间远端参与者以 MIGRATION 原因退出；应用必须撤销旧房间远控授权，并从 Room 重新获取 Participant/TrackPublication。即使本地媒体仍在发送，旧句柄也不能向目的房间发命令。旧队列中的命令被明确中断，迁移回调新提交的命令不被误取消。

迁移 Token 只保存在内部恢复状态，公开事件不包含 Token。后续 rejoin 使用目的房间 Token，并校验目的房间名称；旧房间的显式轨道选择、订阅/禁用/尺寸意图不跨房间自动套用。

已确认的本地 participant SID 和轨道 SID 映射不变、且没有未完成发布或 SDP 协商时，迁移保留原传输和稳定源 ID。否则切换到完整 rejoin，遵守既有源重启策略及恢复预算。重复的同房间/本地 participant 迁移通知只刷新内部 Token，不重复触发迁移回调。

## 区域发现和重定向

`RoomOptions.regions` 控制候选端点选择。Cloud 的 HTTPS 区域发现用于首次连接及完整 rejoin，默认开启；`discovery = false` 可关闭主动发现，自建部署不会请求 Cloud。发现失败保留已有可信候选或当前端点。首次连接在接收有效 JoinResponse 前，允许对可重试的信令失败依次切换到可信候选。原端点先尝试，每个候选最多一次；切换时先关闭旧 WebSocket。收到并接受 JoinResponse 后即停止此路径，后续首次 ICE 建连失败仍向调用方报告错误。

`initial_failover=false` 禁用首次切换。`fallback_endpoints` 是应用提供的有序备用列表，最多 32 项，须同时通过信任策略；`trusted_endpoints` 仅表示允许，不会单独触发访问。有剩余候选时，每个端点的 WebSocket+JoinResponse 阶段受 `endpoint_timeout`（默认 5 秒）限制；最后一个端点可使用剩余总期限。所有候选、发现和 Token 获取共享 `connect_timeout`。401/403、取消、畸形协议及不可重试错误不切换。可通过底层事件 `initial-region-failover` 观察尝试，Room 保持 connecting，只在最终连接成功后触发一次 on_connected。

默认允许原始端点和从 Cloud 原始端点指向 `*.livekit.cloud` 的 TLS 根端点；可用 `allow_cloud_regions = false` 禁用后者。自建多区域需要在 `trusted_endpoints` 中列出精确 URL，含路径和端口；http/ws、https/wss 等价。TLS 降级始终拒绝。服务端下发的候选列表不能扩展应用信任策略。

`JoinResponse.alternative_url` 在同一个连接截止时间内跳转，默认最多三次，并拒绝循环。HTTP 3xx 跳转不被跟随，包括 WebSocket 握手和区域发现，避免隐式转发 Bearer Token。区域列表最多 32 项、响应最多 64 KiB；发现超时默认两秒，并受恢复总期限限制。应用 TokenProvider 收到规范化的实际目标 URL。

## 操作取消、超时与确认

Participant/TrackPublication 的异步操作增加可选末尾参数 `OperationOptions`。旧调用方式继续可用，但函数符号发生变化，预编译 consumer 需要重新链接。

```cpp
livekit::sdk::RoomOptions room_options;
room_options.signal_protocol = 15;
room.join_with_token(url, token, room_options);

std::stop_source cancel;
livekit::sdk::OperationOptions operation;
operation.timeout = std::chrono::seconds(2);
operation.stop = cancel.get_token();
operation.wait_for_ack = true;
auto done = room.local_participant().set_metadata("ready", operation);
// 在应用线程等待，不能在串行 Room 回调里阻塞等待 SDK 操作。
done.get();
```

默认 `signal_protocol = 12` 保持原连接行为。官方服务端在 protocol <15 时不发送元数据成功 RequestResponse，因此此模式下元数据 `wait_for_ack = true` 在发送前返回 `SDKError(unsupported)`。显式选择 15 后可等待确认；不支持确认的旧服务端仍可能超时，不能把版本号当作远端能力保证。目前只允许 12 和 15，未启用 17/18 的 packet trailer、协商数据通道或单 PeerConnection。

默认元数据 future 继续在发送后完成；`wait_for_ack` 仅为元数据设置器增加服务端确认语义，QUEUED 不是最终确认。`publish_track` 原本就等待 TrackPublishedResponse；其它控制操作请求 `wait_for_ack` 时明确返回 unsupported。服务端确认不等于所有远端已观察到变化或媒体已播放。

超时从入队时开始。`timeout = 0` 使用 `connect_timeout` 并限制为最多五分钟；显式值也不得超过五分钟。独立的有界定时器使超时不依赖信令线程是否正在等待 SDP。预先取消的操作不入队；开始发送前取消/超时分别返回 cancelled/timeout，开始发送后返回 outcome_unknown，不能据此推定服务端没有执行。取消不会撤回已经交付的普通数据或元数据。调用者不得自动重放结果未知的业务操作。

取消或超时后的迟到 RequestResponse 被忽略。断开和完整重新入会清理待确认请求；每个传输中的请求 ID 不复用。动态轨道发布若在确认处理前被取消，迟到确认会触发取消发布，不纳入重连期望状态。发布确认和取消并发时可能已产生发布回调，SDK 随后清理；不要把取消当作远端永远看不到临时轨道的保证。发布确认到截止时间仍未收到时，以完整 rejoin 清理不确定状态，只恢复先前已确认的源；禁用重连时终止当前会话。

原生 Create/Set SDP 的等待接受会话 stop_token、当前连接期限和剩余恢复预算；取消/期限检查不会等待固定十五秒后才执行。操作级取消先终结应用 future，正在执行的原生协商仍由会话截止时间与会话停止控制。

## 验证与当前边界

`tests/reconnect_integration.py` 使用隔离的官方 LiveKit Server 和 loopback WebSocket 代理，分别覆盖正常 resume、服务端要求 rejoin、拒绝 resume 后回退、真实服务器进程重启、旧句柄失效、静音/禁用恢复与终止原因。`--rounds` 可增加连续 resume 的轮数；代理检查真实 SDP ICE 凭据变化和 SyncState 次数。

尚未完成剩余 ClientConfiguration 行为、单 PeerConnection 以及跨平台/物理设备验收；真实 Cloud 跨区和服务端房间迁移尚未验收。当前官方服务端 1.13.10 的 MoveParticipant 返回 `not implemented`，RoomMoved 已验证的是官方协议注入及随后真实目标房间的重连/媒体。本地 SFU 对 resume 的支持需要独立检查；如果它明确请求完整重连，会走 rejoin。不能从官方服务端测试结果推定所有部署均支持无缝恢复。
