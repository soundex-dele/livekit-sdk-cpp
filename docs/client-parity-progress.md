# 客户端补齐实施记录

完整目标：[客户端能力补齐方案](../../docs/architecture/cpp-sdk-client-parity-plan.md)。服务端管理 API 不在实施范围。本文记录增量进度，不将部分完成等同于方案完成。

## 当前交付与后续待办（2026-10-27）

已交付连接恢复、操作取消/确认、逐轨 PCM/I420 输入输出，后续增量加入 RGBA/旋转/采集时间、订阅权限/接收质量、DTMF/转写和首次信令 failover。C++↔Go 单声道 PCM/官方数据消息互通已验证；RPC 实现和验证见最后一节。下文历史增量的限制描述保留当时状态；当前剩余范围以本节及最后一节为准。

| 阶段 | 后续实现 |
|---|---|
| P0 | 可选控制回调执行器、统一请求处理和内部模块拆分收尾、完整可观测性接口；初版能力查询已加入，实际逐轨 codec 与硬件能力仍待扩展 |
| P1 | 首次 JoinResponse 后的 ICE/媒体失败切换、剩余 ClientConfiguration 编码及硬件策略、真实 Cloud 验收 |
| P2 | 编码帧输入输出、RTP/RTCP 观察与注入、视频动态尺寸、任意 PCM 块和设备时钟漂移补偿 |
| P3 | VP8 Simulcast、分层启停、实际音频播放时钟驱动的同步、准确解码和呈现统计 |
| P4 | 文本/字节流、文件传输、有界公平数据调度；RPC 本轮验证记录见文末 |
| P5 | DataTrack 发布订阅、分片重组、过期处理、背压及恢复 |
| P6 | 媒体/数据 E2EE、密钥提供器和轮换、跨 SDK 格式兼容；DataTrack 加密依赖 P5 |
| P7 | 单 PeerConnection 模式、剩余传输/编码/日志/统计接口、跨平台发布收尾 |

C++ ↔ Go 双向单声道 PCM、DTMF、转写夹具已经运行通过，编码帧/视频互通仍需补齐。既有可靠/非可靠/定向普通数据已支持，不因 P4/P5 待实现而视为缺失。SVC、GPU 零拷贝及硬件编码器全覆盖属于额外增强，不作为 Go 基线对齐的推定要求。

验收仍缺真实 Cloud 跨区、真实服务端无缝 RoomMoved、新增能力的 Linux/macOS 验证、长期资源稳定性、完整弱网矩阵和实际设备人工验收。RoomMoved 当前证据包含协议注入及真实目的房间重连/媒体；本地官方服务端 MoveParticipant 返回 HTTP 500 `not implemented`。最终独立 CTest 10/10 及 Windows 合成媒体集成通过，不能替代上述验收；并发负载下曾出现的等待契约失败及未确认根因保留在最后一节。

## 2026-10-27：协议基线与信令恢复

已实施：

- 固定 protocol v1.53.0 的真实源码提交 `5f3bf01d03f8242c83eacf6bdb2d77142f7c8ac4`，保存四个原始 schema 及 SHA-256。无网络 descriptor 合约测试检查已使用字段的类型、编号、presence、oneof 及枚举值。
- 修正 UserPacket.topic 的 optional presence 和 DataPacket payload oneof。补入 ReconnectResponse、SyncState 和 DataChannelInfo 子集。
- 通过 CMake 生成公共 version.hpp；信令版本与项目版本统一为 0.3.0，协议声明保持已实现的 12。
- 新增 SDKError，信令错误区分权限、断开、超时、资源上限与恢复失败，保留 HTTP status。401/403 不自动重试，错误不包含 Token。
- 新增 SignalSession，保存刷新 Token、排队 ICE，避免信令短暂断开被 WebRTC 回调错误地视为媒体失败。
- 新增 ReconnectOptions、reconnecting 状态和两个恢复回调。稳定会话中信令断开或心跳超时后，使用 reconnect=1 与 participant SID 恢复原会话，并同步 SDP、已发布轨道、订阅例外、禁用状态及数据通道。
- 有界退避、抖动、总时间预算和可取消等待；恢复原会话保留句柄及原生媒体。恢复成功不重复发 on_connected/on_disconnected。

截至本次后续增量的限制（不能据此宣称 P1 完成）：

- resume 需要服务端明确回复 ReconnectResponse；无法恢复或协商/发布状态不确定时，现已支持回退完整重新入会，不盲目重放未确认命令。
- RoomMoved、区域发现/重定向和外部 TokenProvider 的后续增量见下文；真实服务端迁移尚待支持该功能的端点验收。
- 已接入 ReconnectResponse 中的 ICE server 更新和已协商发布端 ICE restart；ClientConfiguration 等剩余扩展仍待适配。协议 17 和单 PeerConnection 仍未启用。
- P0 的统一请求管理、完整操作级取消、能力查询与可选回调执行器仍未全部交付。
- P2—P7 尚未交付；媒体输入、Simulcast、同步、RPC/streams/DataTrack/E2EE 和平台验收继续保留在目标内。

验证记录：

- 修改前 Windows Release CTest 9/9 通过。
- 修改前官方 LiveKit Server 1.13.10 SDK 集成通过：`out/sdk-integration/20261027-110756/result.json`，包括真实 WAV/Y4M、数据及动态发布订阅。
- 新增 descriptor 合约检查：148 字段通过，并验证改变字段编号会被拒绝。
- 新代码 Windows Release 全量构建通过：`out/client-parity-build-final.log`；CTest 10/10 通过，新增 protocol_contract 纳入默认测试。
- 官方 LiveKit Server 1.13.10 信令故障注入通过：`out/reconnect-integration/20261027-112236/result.json`。两次实际 resume，保留 participant/track SID 和旧 publication 句柄；恢复后合计收到 49 帧视频、2 条可靠消息；第三次在恢复回调中取消，无迟到恢复回调。无效 Token 以不可重试的 401/403 类型错误返回。
- 新代码 SDK 端到端回归通过：`out/sdk-integration/20261027-112302/result.json`，真实 WAV/Y4M、动态发布/取消发布、定向可靠/非可靠消息及原有生命周期检查通过。
- 此阶段尚未执行百轮故障（后续结果见下文）；长时间稳定性、Linux/macOS 和设备人工验收未完成。本轮结果只证明上述 Windows 本机合成媒体路径。

测试入口：

```powershell
cmake --build --preset windows-release
ctest --preset windows-release --output-on-failure
python tests/sdk_integration.py --client out/windows-x64/Release/livekit_sdk_integration.exe --server <livekit-server.exe>
# 使用安装 websockets==15.0.1 的 Python 环境：
python tests/reconnect_integration.py --client out/windows-x64/Release/livekit_reconnect_integration.exe --server <livekit-server.exe>
```

重连集成只对测试进程的 loopback WebSocket 代理注入故障；使用合成视频和无窗口帧回调，不操作桌面，不改全局网络配置。实际设备人工验收没有执行。

## 2026-10-27 后续：完整重新入会与 ICE restart

已实施：

- RecoveryState 保留已确认的动态/初始发布源，恢复名称、stream、source、静音；新增稳定 `TrackInfo.client_id`，完整 rejoin 递增会话 generation，旧句柄不能操作新会话。
- 未确认请求明确失败，取消发布的源从期望状态移除；一次性数据不在 rejoin 时重复发送。
- 远端订阅/禁用/尺寸按 SID 或参与者身份和唯一轨道特征恢复，歧义匹配报错；恢复服务器已确认的本地 metadata/name/attributes。
- 新增 `MediaRestartPolicy`，有限文件/网络输入默认不从头重播，调用者可显式选择 restart 或 fail。
- resume 与 rejoin 共用尝试次数和总截止时间；服务端 Leave 的 action/legacy can_reconnect 按终止原因分类，移除/删除/重复身份不会被重连指令覆盖。
- ReconnectResponse 更新 ICE 配置；恢复时为已协商 publisher 生成真正的 ICE restart offer，并等待相关传输就绪。

验证：

- Windows Release 全量构建通过：`out/client-rejoin-build-final.log`；CTest 10/10 通过。
- 双向媒体 resume 通过：`out/reconnect-integration/20261027-113742/result.json`，两次恢复、两次 SyncState、三组不同 publisher ICE 凭据；视频、可靠数据和原句柄可用。
- rejoin 通过：`out/rejoin-integration/20261027-113807/result.json`，覆盖直接重新入会、STATE_MISMATCH 拒绝 resume 后回退、发布重建、旧句柄拒绝、静音/禁用/metadata 恢复、已删除源不复活，以及踢人优先于重连指令。
- 真实隔离服务器进程终止并重新启动通过：`out/server-restart-integration/20261027-113957/result.json`。两个 SDK Room 自动恢复，跨服务器状态丢失后真实视频和可靠数据继续交付。
- 百轮连续 resume 通过：`out/reconnect-integration/20261027-114136/result.json`，100 次恢复、100 次 SyncState、101 组不同 publisher ICE 凭据、1,566 帧视频和 100 条可靠消息；最终恢复中取消通过。长期内存/线程稳定性和物理设备仍未验收。
- 完整 rejoin 修改后的原有 SDK 回归通过：`out/sdk-integration/20261027-114712/result.json`，真实录制校验、动态发布/订阅/取消发布、定向数据和会话重用均通过。

使用说明与边界见 [连接恢复](reconnection.md)。全部 P0—P7 目标继续有效，以上不代表全方案完成。

## 2026-10-27 后续：异步 TokenProvider

- 新增公共 TokenRequest/TokenResult/TokenProvider；空初始 Token 可异步获取，HTTP 401 每次连接尝试最多刷新一次，403 不触发刷新，服务端下发的刷新 Token 仍优先使用。
- provider 收到单调时钟 deadline 和 stop_token。SDK 有界等待、取消传播、最多一次接收完成；超时/取消后的迟到结果不接触已释放的会话。provider 启动函数须及时返回，应用负责异步任务和资源所有权。
- Token 获取、WebSocket 建连和 JoinResponse 共用单次连接截止时间，恢复期间还受总恢复预算和原会话 duration 限制。provider 异常不直接写入错误信息，避免泄漏凭据。
- 新增异步/内联完成、重复完成、超时、取消、迟到完成、空/超大结果和错误脱敏单测，以及真实服务端初始获取、401 刷新及拒绝无限刷新集成。
- Windows Release 全量构建通过：`out/client-token-provider-build.log`；CTest 10/10 通过。官方服务端验证通过：`out/reconnect-integration/20261027-115359/result.json`，含 TokenProvider 和双向媒体 resume。

## 2026-10-27 后续：可信区域与信令重定向

- `RegionOptions` 提供 Cloud 区域发现开关、Cloud 目的地址策略、精确可信端点列表、发现超时和重定向次数限制。自建部署不发起 Cloud 发现请求。
- 完整 rejoin 时发现 HTTPS `/settings/regions` 候选，维护短期会话缓存；发现失败保留已验证候选或原端点。支持服务端 Leave.regions 更新，在恢复预算内尝试候选；resume 始终连接当前端点。
- JoinResponse.alternative_url 只在信任策略通过后使用，限制次数和循环，禁止 TLS 降级。TokenProvider 看到的是规范化后的实际目标端点。
- IXWebSocket 11.4.6 内部默认携带 Authorization 跟随 HTTP 重定向且没有公开策略接口；使用可审计 CMake 适配生成私有构建副本，禁用 HTTP 重定向，未修改下载的上游源码或服务端管理 API。
- HTTP 区域响应和候选数量有界；发现支持 stop_token 和单调 deadline，不跟随 HTTP 重定向。新增 URL/信任策略单测、官方服务端重定向集成、独立 HTTP 超时/取消/恶意响应测试。
- Windows Release 全量构建通过：`out/client-regions-build-final.log`；CTest 10/10 通过。
- 可信区域/HTTP 边界集成通过：`out/region-integration/20261027-121402/result.json`；真实服务端重定向入会后发布成功，未授权/循环/HTTP 跳转被拒绝，禁止目的地址请求次数为 0；HTTP 有界响应、超时和取消通过。
- 服务端 Leave.regions 故障注入通过：`out/region-rejoin-integration/20261027-121414/result.json`；切换到可信备用信令路径，后续 resume/rejoin 使用该路径，2 次 rejoin 后 80 帧视频和 2 条可靠消息交付，保留稳定源 ID、拒绝旧句柄、移除源不复活、终止原因优先等检查均通过。该测试使用本机两个信令入口和真实官方服务端，不等同于真实 Cloud 跨区验收。
- 本次区域/Token 增量后的原有 SDK 集成回归通过：`out/sdk-integration/20261027-121614/result.json`，录制校验、动态轨道、可靠/非可靠定向消息和会话重用通过。
- 测试夹具首次运行发现服务器监听端口开放早于路由节点就绪（HTTP 500），现以真实 JoinResponse 确认就绪后运行被测客户端；HTTP 取消用独立 asyncio HTTP 服务验证，避免 WebSocket 测试服务器在对端关闭后发送响应产生断言日志。
- 边界：自动发现目前用于已建立会话的完整 rejoin，首次连接失败仍报告错误；Cloud 真实跨区部署、RoomMoved 和剩余 ClientConfiguration 行为尚未验收。

## 2026-10-27 后续：RoomMoved 与服务端传输策略

- 增加 RoomMovedResponse wire 消息及 `RoomCallback.on_room_moved`。迁移前校验完整参与者快照，原子更新房间信息、本地和远端参与者；旧远端发 MIGRATION 退出事件。迁移边界使旧句柄失效，旧排队命令被中断，迁移回调中新提交的命令保留。
- 新 Token 仅保存在内部恢复状态，不放入事件；后续 rejoin 校验目的房间，旧房间的订阅设置不带到新房间。本地已确认映射稳定且协商已完成时保留传输，否则重建。相同迁移通知不会重复使句柄失效。
- RoomMoved 代码 Windows Release 构建通过：`out/client-room-moved-build.log`；CTest 10/10 通过，含新增状态事务、队列清理、旧句柄及稳定源 ID 检查。
- 协议注入加真实目标房间 rejoin 通过：`out/room-moved-integration/20261027-122914/result.json`；迁移回调一次、rejoin 一次、目的房间 12 帧视频和 1 条可靠消息；代理验证 rejoin 确实使用目的房间 Token。
- 真实 MoveParticipant 尝试未通过：`out/room-moved-integration/20261027-122754/server.log` 明确返回 HTTP 500 `not implemented`（测试控制 Token 已带 roomAdmin 和 destinationRoom）。因此上述成功不能表述为真实服务端无缝迁移已验收。`--mode server` 保留真实验收入口，不静默回退注入。
- 已接入 ClientConfiguration.force_relay 和 resume_connection：应用 relay_only 不被服务端关闭，强制中继与 disable_turn 冲突明确失败，禁用 resume 后走完整 rejoin；ReconnectResponse 省略配置时保留既有策略。Windows 全量构建通过：`out/client-configuration-build-retry.log`（前一次生成过程退出且确认无存活编译进程后重试）。
- 修复迁移时原生 OnTrack 早于目的房间快照到达的竞态：保留目的快照中已观察到的订阅，并通过新句柄补发订阅回调，不保留旧房间其它订阅。SDK 与相关 consumer 增量编译通过：`out/client-migration-race-build.log`、`out/client-migration-race-consumers.log`；当前 CTest 10/10 通过。
- 中继冲突及区域边界通过：`out/region-integration/20261027-125118/result.json`；禁用 resume 故障注入通过：`out/resume-disabled-integration/20261027-125135/result.json`，0 次 resume 请求、2 次完整 rejoin、78 帧视频和 2 条可靠消息。
- 迁移快照中轨道映射缺失时的重建通过：`out/room-moved-integration/20261027-125152/result.json`，迁移自动重建及后续断线共 2 次目的房间 rejoin、13 帧视频、1 条可靠消息；稳定源 ID、旧句柄拒绝和目的 Token 检查通过。
- 普通信令恢复回归通过：`out/reconnect-integration/20261027-125224/result.json`，2 次 resume、真实 ICE restart、TokenProvider 及取消检查通过。
- 原有 SDK 集成回归通过：`out/sdk-integration/20261027-125235/result.json`，动态发布/订阅、可靠/非可靠数据、录制校验和会话重用均通过。
- P0 的完整请求/执行器/能力底座及 P2—P7 仍待交付。ClientConfiguration 的 codec/hardware 设置、真实 Cloud 跨区与迁移、长期资源稳定性和跨平台/设备验收继续保留。
- 本阶段曾遗留原生 SDP future 的固定 15 秒等待；后续收紧及验证见下一节，不能以网络、Token 和退避测试替代原生等待测试。

## 2026-10-27 后续：操作期限、取消与服务器确认

- 新增 `OperationOptions`：操作级 timeout/stop_token 和元数据 `wait_for_ack`，保留旧调用的默认完成时机。开始发送前取消/超时返回 cancelled/timeout，开始发送后返回 outcome_unknown；不把应用取消理解为撤回服务端操作。公共签名变化要求预编译消费者重新链接。
- `Command` 原子状态仲裁完成/取消；每个 Room 一个惰性有界定时器，信令/SDP 等待期间也能完成应用 future。排队、待确认请求有容量限制，超时最多五分钟。PendingRequests 负责请求 ID、QUEUED 非最终状态、错误映射、断开中断和迟到响应忽略。
- 动态发布取消后，迟到 TrackPublishedResponse 清理源和原生轨道；截止时间仍无确认则完整 rejoin，只重建已确认的期望状态。未确认发布在断开后返回 outcome_unknown，不静默重放。发布回调与取消并发时可能先观察到发布再清理，这不是撤回远端观察的保证。
- 原生 SDP future 改为会话 stop_token 和单调 deadline 有界等待；CreateDescription/SetLocalDescription 共享期限，受连接/恢复总预算限制。单测覆盖取消、到期、迟到完成、到期时已有结果和 deferred future 拒绝。
- 实测 protocol 12 元数据成功确认会超时：`out/operations-integration/20261027-130735/`。官方服务端代码要求 protocol >14 才发送 OK RequestResponse。新增 `RoomOptions.signal_protocol = 15` 显式选项；默认仍为 12，默认模式请求元数据确认会在发送前返回 unsupported。没有直接上调到 17，也不把 protocol 15 当作未知服务端能力保证。
- 官方服务端操作集成通过：`out/operations-integration/20261027-131631/result.json`。真实成功/权限拒绝 ACK、取消、丢 ACK 超时、迟到 ACK、预取消不发送、取消发布清理、丢发布 ACK 后重建、两次 rejoin 和 48 帧媒体通过。QUEUED 与其中一条拒绝响应由代理注入；所有成功 ACK 来自真实服务端，另有直接连接的真实权限拒绝。
- protocol 15 恢复通过：`out/reconnect-integration/20261027-131631/result.json`，2 次 resume/SyncState、3 组 publisher ICE 凭据、49 帧和 2 条消息；记录中的 ConnectionClosedError 来自预期故障切断，测试夹具随后将正常关闭与真实代理错误分开。
- protocol 12 默认路径回归通过：`out/reconnect-integration/20261027-131710/result.json`，2 次 resume/SyncState、50 帧和 2 条消息，代理错误为 0。操作/原生等待单测纳入 SDK 测试，CTest 10/10 通过。
- 本轮最终 Windows Release 全量构建通过：`out/client-operations-complete-build.log`，包含 SDK、CLI、示例与全部测试消费者；全量重建后 CTest 再次 10/10 通过。原有 SDK 集成通过：`out/sdk-integration/20261027-131710/result.json`，动态轨道、定向数据、会话重用及实际 WAV/Y4M 校验通过。
- 最终构建 protocol 15 resume 回归通过：`out/reconnect-integration/20261027-132039/result.json`，两次恢复、50 帧、2 条消息，测试代理没有非预期错误。
- 最终构建操作集成再次通过：`out/operations-integration/20261027-132121/result.json`，真实 ACK/拒绝、取消与超时、迟到响应、未确认发布不重放、两次 rejoin 和 48 帧检查通过；其客户端 SHA 与上述最终 resume 测试一致。
- 本增量仍不代表 P0/P1 完成。能力查询、回调/媒体执行器隔离、其余 ClientConfiguration、首次 Cloud failover 和 P2—P7 继续待实现。操作协议测试未替代跨平台、长时资源测试及物理设备人工验收。

## 2026-10-27 后续：有界视频 pull 输出

- 新增公共 `VideoFrameStream` 和 `RoomOptions.video_frame_stream`。应用线程按需读取不可变、拥有像素的 I420 帧；容量按帧数和像素字节双重限制，满时淘汰旧完整帧，超大帧丢弃。SDK 不等待消费者读取，不在该输出路径调用应用媒体处理函数。
- 增加读取取消/超时、close 唤醒及队列统计；一次绑定一个逻辑 Room，拒绝跨 Room 共享和已关闭流复用。关闭输出不离开 Room，Room 终止会关闭输出。现有串行 `video_frame_handler` 保留，与 pull 输出互斥。
- `VideoFrame.stream_generation` 标识传输/房间代次。完整 rejoin、RoomMoved 清除旧队列；移除轨道清理对应帧。原生转换进行中发生移除/重建或迁移时，通过 track incarnation 检查防止旧帧以新代次入队。应用已取走的帧独立存活，由应用决定何时丢弃。
- 新增容量、独立字节上限、取消/关闭、所有权、过大帧、旧代次及转换中移除/重建的测试。全量 Windows Release 构建通过：`out/client-video-stream-build.log`；新增竞态测试与公共 API 示例构建通过：`out/client-video-stream-consumers.log`；最终 CTest 10/10 通过。
- 官方服务端 pull 集成通过：`out/video-stream-integration/20261027-132927/result.json`，实际接收 39 帧，主动不读取时保持队列上限并正常完成元数据 ACK 和数据事件；完整 rejoin、旧代次清理、轨道取消发布清空、单 Room 所有权和关闭后持有帧有效均通过。重连测试只注入 Leave 指令，重建连接及媒体来自真实服务端。
- 原有 SDK 集成回归通过：`out/sdk-integration/20261027-133212/result.json`，动态轨道、数据、会话重用及 WAV/Y4M 校验通过；操作取消/确认回归通过：`out/operations-integration/20261027-133212/result.json`，两次 rejoin 和 47 帧，代理错误为 0。
- 使用说明见 [有界视频帧读取](video-stream.md)，可编译示例 `examples/video_stream/main.cpp`。字节上限仅约束队列像素，不包括解码器、转换临时缓冲、每轨原生最新帧或应用持有帧。CPU 转换仍在会话线程；本增量解决慢应用消费的隔离，不宣称整个解码/转换流程脱离信令线程。
- 这是 P0 媒体隔离与 P2 输出的一部分，尚无 PCM pull/输入、原始视频输入、编码帧接口或音画同步。自定义控制回调执行器、能力查询、其余 P1/P2—P7 目标仍保留；未执行实际设备人工验收。

## 2026-10-27 后续：逐轨 PCM16/float32 pull 输出

- 新增 `AudioFrameStream`、`RoomOptions.audio_frame_stream`、不可变 `AudioFrame` 和按 track SID 读取。远端原生 AudioTrack sink 经媒体图 `AudioFrameOutputNode` 独立复制每轨解码 PCM，不经过应用混音或默认麦克风。输出可选交错 PCM16 或归一化 float32，保留实际采样率、声道、每声道样本数及 duration。
- 全流队列受块数/样本字节双重限额，慢读取不等待消费者。块序号和每声道 sample_index 在入队前递增，应用能识别被丢弃块造成的间隙；格式变化会清除旧格式样本并建立新 track_generation。保留原生可选捕获时间估计，不伪造 UTC、RTP 时间或声卡播放位置。
- 独立轨道 lease 拒绝移除后迟到的旧 sink 回调；支持读取消/超时、关闭唤醒、单 Room 绑定、轨道移除清理和完整 rejoin/RoomMoved 的旧代次清理。关闭输出不关闭 Room，Room 结束关闭输出，应用已经取得的样本独立存活。
- 新增双轨正负样本隔离、独立字节上限、丢样本位置、格式变化、旧 lease、代次切换、按轨等待、float32 精确转换、取消与关闭单测。Windows Release 全量构建通过：`out/client-audio-stream-build.log`；CTest 10/10 通过。公共消费者 `livekit_audio_stream_example` 和测试消费者构建通过：`out/client-audio-stream-consumers.log`。
- 首次双音调集成未通过频率检查。诊断 `out/audio-stream-integration/20261027-141406/pcm-diagnostics.json` 及原始 PCM 表明：约前半秒为有效音频到达前的解码静音，之后每 100 ms 有 44/88 个周期。测试改为两轨均出现有效音频后测量，不改变频率阈值，测量期间的静音/间隙仍计入。诊断构建和修正构建记录为 `out/client-audio-diagnostics-build.log`、`out/client-audio-warmup-build.log`。
- 官方服务端最终双音轨集成通过：`out/audio-stream-integration/20261027-141853/result.json`，实测 440.0/880.0 Hz，266 个解码块；慢读取造成 62 块淘汰但元数据 ACK 正常；PCM16/float32、轨道独立、Room 独立、一次真实 rejoin 后的新代次、轨道移除清理和已取得样本的寿命均通过。测试代理只注入 Leave，媒体传输/解码来自真实官方服务端。
- 视频 pull 回归通过：`out/video-stream-integration/20261027-141922/result.json`，39 帧、一次 rejoin、慢消费者隔离、移除及关闭清理通过。
- 原有 SDK 集成通过：`out/sdk-integration/20261027-141922/result.json`，动态发布/订阅/取消发布、定向数据、会话重用与实际 WAV/Y4M 录制校验通过；`git diff --check` 无空白错误。
- 使用说明：[逐轨 PCM 读取](audio-stream.md)；公共 API 示例：`examples/audio_stream/main.cpp`。这些是 Windows 本机合成媒体自动化结果，不是物理设备、Go 互操作或跨平台验收。
- P2 仍未完成：本地 PCM/原始视频输入、重采样输入策略、编码帧及 RTP/RTCP 扩展继续待实施。P0 控制执行器/能力查询、P1 剩余恢复与配置，以及 P3—P7 的全部剩余范围继续有效。

## 2026-10-27 后续：应用 I420 视频输入首版

- 新增公共 `I420VideoSource`、`I420SourceOptions`、`VideoWriteStatus`、统计及 `MediaSpec.video_source`。应用提交 CPU I420，内部复制可见像素、去除 padding；固定偶数尺寸、正 stride，逐 plane 校验完整性，不将帧装入 JSON 命令。
- 写队列按帧数/紧凑像素字节双限额；未激活或队列满返回 backpressure，永久 close 返回 closed，消费/写入时淘汰超龄帧。源在媒体图就绪后开始接收，按 MediaSpec.fps 限制最大取帧速率；停止可唤醒空队列消费者并中断 pacing。
- 发布构造时排他绑定 source，拒绝同 Room/跨 Room 同时使用；取消发布或销毁后清空并释放，允许随后再次发布。完整 rejoin 保留已确认源对象，重新激活递增输入 generation，旧队列不回放，旧 publication 句柄失效且稳定 client_id 保留。未确认发布沿用既有不重放规则。
- 输入使用提交时的本地单调时钟，转换为 WebRTC 时间基；不采用 VideoFrame 的输出时间戳。外部采集时间、RGBA/旋转转换、动态尺寸和 GPU buffer 仍未实现。排他绑定在完整 rejoin 时暂时释放，应用不得在恢复期间将同源改发其它 Room；close 是永久 EOS，应显式取消发布，关闭的源不参与完整恢复。
- 新增带 padding/末行无 padding、精确像素复制、队列/字节边界、过期淘汰、取消、停止/close 唤醒、重绑定及不完整 plane 单测。Windows Release 全量构建通过：`out/client-video-source-build.log`；CTest 10/10 通过。公共示例和集成测试最终构建通过：`out/client-video-source-pixel-check-build.log`。
- 首次收发失败记录：`out/video-source-integration/20261027-143916/`。诊断运行 `out/video-source-integration/20261027-144248/client.stderr.log` 显示实际 320x180、Y/U/V=76/98/179，输入为 76/94/180；原测试对有损 VP8 要求单点偏差≤3，色度误差 4 超限。测试增加编码前所有像素精确检查，远端改为所有解码像素偏差≤8/255；没有跳过远端图像检查。诊断构建记录：`out/client-video-source-diagnostics-build.log`。
- 官方服务端最终集成通过：`out/video-source-integration/20261027-144507/result.json`，41 帧远端视频、52 次接受、583 次背压；编码前精确像素、远端全帧容差、输入压力下元数据 ACK、一次真实 rejoin、稳定源 ID/旧句柄失效、重复发布拒绝、取消后再发布、EOS 及无输入源退出均通过。代理仅注入 Leave，入会和媒体传输来自真实官方服务端，代理错误为 0。
- 原有 SDK 集成回归通过：`out/sdk-integration/20261027-144156/result.json`，动态轨道、定向消息、会话重用及实际 WAV/Y4M 校验通过；`git diff --check` 无空白错误。
- 使用说明：[应用 I420 视频输入](video-source.md)；可编译示例 `examples/video_source/main.cpp`。MediaSpec 新增字段，预编译消费者需要重新编译/链接。本轮未进行 Go 互操作、跨平台或物理设备人工验收。
- P2 仍待：PCM 输入/重采样、外部时钟、其它原始视频格式、编码帧及 RTP/RTCP 接入。P0/P1 未完成项和 P3—P7 全部剩余范围继续有效，完整方案仍在实施。

## 2026-10-27 后续：逐轨 PCM 输入、重采样与立体声

- 新增 `PcmAudioSource`、`PcmInputFrame`、`PcmSourceOptions`、`AudioWriteStatus` 和 `MediaSpec.audio_source`。接受交错 PCM16/float32、8/16/24/32/44.1/48/96 kHz、单/双声道；一次写入为 10..100 ms 的 10 ms 整数倍，验证格式、样本完整性和有限归一化浮点。每次写入全部成功或全部拒绝，不部分推进队列或采样时间线。
- 输入队列按时长/内部 PCM16 字节双重限额，满或未激活返回 backpressure；无回调供帧，不在 JSON 中传样本。逐轨独立 WebRTC sinc 重采样器输出 48 kHz/480 个每声道样本，通过独立原生 AudioTrack 及 Opus 编码器发布，不混入麦克风 ADM。原先固定单声道的内部 AudioSource::push 改为保留 PcmFrame 的声道及可选采集时间。
- 可选 steady_clock 首样本采集时间，连续省略时自动推进；显式时间重叠/超出实时窗口被拒绝。10 ms 时间网格处理缺口、欠载和迟到，缺口补零后通过同一滤波器；调度落后超过 20 ms 时跳过旧时隙并重置滤波器。accepted/consumed/dropped/silence/skipped 单独统计，未承诺高负载无损或硬实时。
- 排他源绑定、停止/close 唤醒、EOS、取消发布清理及完整 rejoin 接入既有恢复契约；仅恢复已确认源，清除旧队列与时间线、重建滤波器并递增 generation，稳定 client_id 保留，旧 publication 失效。格式变化需新源；任意 PCM 块边界、额外采样率、设备时钟漂移补偿仍非本增量能力。
- 为 PCM 双声道增加经上游描述符检查的 AddTrackRequest.stereo 字段。首次集成 `out/audio-source-integration/20261027-150625/` 显示服务端轨道 stereo=true，但远端只输出单声道；诊断 `out/audio-source-integration/20261027-151153/audio-sdp.jsonl` 证明发布端答案已包含 stereo=1。根因为内置 Opus 解码工厂默认只声明单声道；现按后端实际支持声明双声道接收，并增加能力声明单测。默认 Opus 接收可能将单声道解码为重复双声道，应用须使用实际 AudioFrame.channels。
- 新增输入队列/字节边界、整块背压、浮点精确转换、无效输入、采集时间缺口/重叠、欠载、取消/停止/关闭、重绑定单测；七种采样率各重采样 1 kHz 信号，验证样本数、频率、左右不串扰及滤波器重置。
- Windows Release 初次全量构建通过：`out/client-audio-source-build.log`；立体声修正构建通过：`out/client-audio-stereo-codec-build.log`、`out/client-audio-stereo-consumers-build.log`；所有消费者最终重新链接通过：`out/client-audio-source-final-build.log`。
- 官方服务端 PCM 输入集成通过：`out/audio-source-integration/20261027-151533/result.json`。16 kHz PCM16 单轨和 44.1 kHz float32 双声道实测为 441/660/883 Hz；一次真实 rejoin 后为 440/660/881 Hz，共 552 个远端块。背压、双轨及左右独立、稳定源 ID、旧句柄拒绝、欠载期间元数据确认、取消一轨不影响另一轨、EOS 和无首块源退出均通过；代理错误为 0。运行中记录到调度跳过，不能据此声称重载下无丢样。
- 原有 SDK 集成通过：`out/sdk-integration/20261027-152212/result.json`，动态轨道、定向消息、会话重用及实际 WAV/Y4M 校验通过；原有 PCM 输出回归通过：`out/audio-stream-integration/20261027-152212/result.json`，440/880 Hz、348 块、双格式/多 Room、rejoin 及清理通过。
- 并行运行 CTest 与两个媒体集成时，一次 sdk 测试报 `Native wait contract failed`；原日志没有具体断言信息，尚未证明根因。未改生产等待逻辑或放宽 500 ms 阈值，独立复验通过；增加断言行号及超时/取消实际耗时诊断，构建记录 `out/client-audio-source-wait-diagnostics-build.log`。最终独立 CTest 10/10 通过。并发高负载时延与长期资源稳定性仍保留待验收，不能用最终通过抹去该次失败。
- Go 互操作准备：已在 `out/toolchains/go1.26.10/` 安装独立 Go 1.26.10，官方 archive.json SHA-256 为 `7ecd2d7260e82c59bcbd7f1077a07cee2dea607295b858df67195a1e4e09629b`，未修改全局 PATH。当前 Go SDK go.mod 要求 Go 1.26.8。默认 proxy.golang.org 访问超时，命令局部使用 https://goproxy.cn 成功获取 media-sdk v0.1.1，模块及 go.mod 校验值均与本地 Go SDK go.sum 一致，未关闭校验。已确认 `D:/msys64/mingw64/bin/gcc.exe` 15.2.0、pkg-config 及 Opus 1.5.2 可用；Go PCM 依赖 CGO，互操作程序仍待编写/构建/运行。Go PCMRemoteTrack 源码明确标注立体声不支持，不能将本次 C++ 立体声结果推定为 Go 互通。
- 使用说明：[应用 PCM 输入](audio-source.md)；可编译示例 `examples/audio_source/main.cpp`。上述为 Windows 本机合成媒体自动化，未执行物理设备、跨平台或 Go 实际互通。整个方案仍在实施：P2 编码帧/RTP/原始视频扩展、P0/P1 余项和 P3—P7 均继续有效。

## 2026-10-27 后续：Go 互通、官方数据消息及权限/视频扩展

- 新增可重复构建的 Go 互通夹具，单独在 out/ 构建，不修改对照 Go SDK，不作为 C++ SDK 运行时依赖。增加 `disable_red` 发布选项，并在完整 rejoin 中保留；Go PCMRemoteTrack 仅支持 Opus，测试关闭 RED。Go 尚不支持立体声 PCM reader，因此只声明本轮单声道互通。
- 官方 DTMF/转写：公开 typed API 和回调，真实 DataPacket oneof，UTF-8/段数/文本及包大小限制，区分发送者与被转写参与者。future 是本地提交完成，不保证服务器转发；不在重连时重放。
- 初次转写互通失败确认是官方服务端仅转发 Agent 发出的转写；测试改用既有 Token kind API 设置 Agent，未增加管理 API。最终通过：`out/go-interop-integration/20261027-164619/result.json`，C++→Go 437.5 Hz（目标 440）、Go→C++ 880 Hz，双向 DTMF 及多语言 partial/final 转写通过。
- `I420VideoSource` 增加显式 steady_clock 采集时间、RGBA CPU 输入、0/90/180/270 度旋转和竖屏输入转横屏输出。输出尺寸固定，BT.601 limited range，忽略 alpha；严格单调时间与实时窗口校验，重绑定清空时间线。没有动态尺寸、GPU 零拷贝或设备时钟漂移补偿声明。
- RGBA 红色测试曾错误期待 V=240；固定 libyuv 的实际值为 YUV 82/90/239。保留 `out/video-source-integration/20261027-164954/` 失败记录，修正精确期望，没有放宽编码前精确像素校验或远端 VP8 容差。
- 增加订阅权限和权限变更事件，逐轨授权使用稳定 client_id；发布前及 rejoin 期间先拒绝缺失映射，收到 TrackPublished 的新 SID 后再授权。支持运行时撤销/恢复；接收端增加 quality，和 dimensions 按最后一次设置覆盖。没有声明 Simulcast 发送。
- 官方服务端 RGBA/权限集成通过：`out/video-source-integration/20261027-165647/result.json`，50 个远端帧、87 次接受及 404 次背压；验证像素、旋转、拒绝第三参与者、rejoin 权限映射、撤销/恢复、稳定源 ID、再发布/EOS。
- 使用说明：[官方数据消息](data-messages.md)、[订阅权限](subscription-permissions.md)、[视频输入](video-source.md)。以上均为 Windows 本机无窗口合成媒体自动化，未做物理设备验收。

## 2026-10-27 后续：首次信令连接切换

- `RegionOptions.initial_failover/endpoint_timeout/fallback_endpoints` 支持首次 Cloud 发现及显式备用端点，信任列表与候选列表分开。只在接受 JoinResponse 前重试；总 connect_timeout 共享，有其它候选时使用单端点预算，最后一个候选使用剩余总预算。
- 不重试 401/403、取消、协议畸形及其它不可重试错误；provider 授权失败不会被发现阶段吞掉。原地址优先、禁止重复候选、切换前关闭旧连接，不发送 on_reconnecting。
- 测试最初假定 join 返回前 on_connected 一定已执行；该假定不成立，现等待异步回调，不修改生产时序。官方服务端及故障代理通过：`out/region-integration/20261027-171149/result.json`，503/静默超时切换、授权/畸形终止、禁用策略、共享截止时间、取消及可信目的检查通过，forbiddenRequests=0。
- 首次 JoinResponse 后 ICE/媒体失败切换、真实 Cloud 跨区、剩余 codec/hardware ClientConfiguration 仍待实现/验收。

## 2026-10-27 后续：官方 RPC

- 新增 `Room::register_rpc_method/unregister_rpc_method`、`LocalParticipant::perform_rpc`、RpcError/Invocation/Result/Completion，采用官方 request/ack/response v1。处理器由独立有界线程启动，完成函数可跨线程调用；ACK、响应截止时间及取消独立于信令线程。
- 对方身份校验、响应早于 ACK、重复请求/完成抑制、UTF-8/大小限制、容量限制、错误脱敏、发送失败及未发送/已发送取消语义已实现。缓存 incarnation 隔离旧完成函数，即便同一个 key 在相同传输代次被移除后重用也不会误完成。
- reconnect/rejoin、RoomMoved、对方退出和会话结束清理待处理 RPC；接收侧 stop 回调在内部锁外执行。不重放；取消后远端可能已经执行。完整语义及上限见 [RPC](rpc.md)。尚不支持压缩 RPC，也未实现跨数据类型公平调度。
- 首次完整构建的编译阶段通过，但后处理依赖复制进程异常退出，保留 `out/client-rpc-build.log`；受影响消费者重试成功：`out/client-rpc-consumers-retry.log`。不将首次构建记录表述为完整成功。
- CTest 10/10 通过：`out/client-rpc-ctest.log`，含新增 RPC 生命周期、伪造身份、容量、重复、取消和七秒 ACK 超时测试。
- 官方 LiveKit Server 1.13.10 与固定 Go 2.19.0 双向互通通过：`out/go-interop-integration/20261027-174515/result.json`。双向 UTF-8 echo、自定义错误码/数据、未知方法 1400，并回归 PCM、DTMF、partial/final 转写；C++→Go 438.5 Hz、Go→C++ 879.5 Hz。C++ SHA-256 `832733e835d48551869759cbba4f475261deacbfa82929587c7ffb1b94dfe192`，Go `704669a32f839dfa2f1265381153bd4739967b25f9805296f7c032ede7fedebd`。后续能力查询及额外边界修正需使用重建后的二进制再次验证。

## 2026-10-27 后续：公共能力查询与 RPC 收尾验证

- 新增 `sdk_capabilities()` / `Room::capabilities()`，区分公开 API 实现、本次入会信令版本/双 PC/primary/relay 配置、服务端 ServerInfo 与发布 codec 策略。未知保留 unknown/nullopt，不根据服务端协议号升级本地声明，不将服务端 codec 列表当成实际编码/协商结果；不初始化设备、不输出凭据。新增 schema 字段仍受固定上游 descriptor 合约检查。
- 当前查询未提供每轨实际 codec、硬件设备能力或服务端通用功能探测；这些能力不能从 supported 本地实现标记推定。rejoin 清空旧代信息，resume 替换新 ServerInfo，RoomMoved 清除旧节点声明，断开查询不返回旧会话有效配置。说明见 [能力查询](capabilities.md)。
- RPC 增加压缩响应明确拒绝、入站容量耗尽后的错误回复、缓存淘汰后同 key 的迟到完成隔离、停止回调重入以及房间迁移清理回归。独立 CTest 10/10 通过，13.73 秒：`out/client-capabilities-ctest.log`。
- Windows Release 协议/SDK 及完整消费者构建成功：`out/client-capabilities-protocol-build.log`、`out/client-capabilities-sdk-build.log`、`out/client-capabilities-full-build.log`。新纯数据夹具初次执行 `out/go-rpc-interop/20261027-175437/` 仍使用不含新模式的旧对象，报 `invalid stoi argument`；重编译夹具成功：`out/client-rpc-only-fixture-build.log`。后续证据均记录新二进制 SHA，未把该次失败当成网络错误或修改生产协议来绕过。
- 双方无音视频发布的 C++↔Go RPC 通过：`out/go-rpc-interop/20261027-175636/result.json`，确认 RPC 可主动建立发布端数据通道，双向 echo/自定义错误/未知方法通过。
- 新二进制完整 Go 互通通过：`out/go-interop-integration/20261027-175641/result.json`，C++→Go 438.5 Hz、Go→C++ 880 Hz；RPC、DTMF、partial/final 转写及 ServerInfo/入会/断开能力快照检查通过。C++ SHA-256 `2a627ca2466a9c41d56e21956eaebb7e4df9936b8a13c5b0b5835b9e39d953d0`，Go `ea17a74e331fec661ac9e92e2f135860eca285b96a6439e3f5ccebde61deaf74`；Go 对照 checkout 仍干净、revision 不变。
- 原有 SDK 集成回归通过：`out/sdk-integration/20261027-175713/result.json`，动态发布/订阅/取消、定向数据、会话重用及 WAV/Y4M 内容校验通过。`git diff --check` 无空白错误。
- 服务端管理 API 未扩展。剩余重点仍为文本/字节流/文件及公平调度、DataTrack、E2EE、Simulcast、编码帧/RTP、音频播放时钟同步和初始媒体失败切换；其它 P0/P1/P7 收尾见顶部表格。以上验证限 Windows 本机无窗口自动化，跨平台、物理设备和真实 Cloud/迁移验收尚未完成，整个方案仍在实施。
