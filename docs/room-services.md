# 新 Room 的 RPC、流、数据轨和 E2EE

这些能力属于 `livekit::sdk::client`，通过 `Room::local_participant()` 或 `MediaEngine::local_participant()` 使用。它们共用新 Room 的连接、认证和恢复流程，不依赖旧 RoomEngine、RoomServices、GraphEngine 或完成回调。构建目标为 `LiveKit::Client`，网络实现需要 `LIVEKIT_BUILD_RTC_NATIVE`。

## 同步接口

| 能力 | 接口 | 返回语义 |
| --- | --- | --- |
| RPC 注册 | LocalParticipant::register_rpc / unregister_rpc | void；失败在调用函数直接记录日志 |
| RPC 请求 | LocalParticipant::perform_rpc | Result<string>，等待远端响应；协议错误保留在 FailureInfo.backend_reason/backend_data |
| 流注册 | LocalParticipant::register_stream / unregister_stream | text 区分文本与字节流，topic 区分业务；void |
| 打开流 | LocalParticipant::open_stream | Result<shared_ptr<StreamWriter>> |
| 发送文本或文件 | LocalParticipant::send_text / send_file | Result<StreamInfo>，等待所有分片及结束消息提交 |
| 流写入、结束 | StreamWriter::write / close | void；同步等待本地提交，失败记录日志，status() 可查询结果 |
| 流读取 | StreamReader::read / read_all / read_all_text | Result；正常 EOF 为 nullopt，异常终止为 Failure |
| 数据轨发布 | LocalParticipant::publish_data_track | Result<LocalDataTrack>，等待服务端发布确认 |
| 数据轨发现 | LocalParticipant / RemoteParticipant::data_tracks | 当前轨道句柄列表 |
| 数据轨订阅 | RemoteDataTrack::subscribe | Result<shared_ptr<DataTrackStream>>，等待服务端分配接收句柄 |
| 数据轨写入、撤销 | LocalDataTrack::write / unpublish | void；失败直接记录日志 |
| 数据帧读取 | DataTrackStream::read | Result<optional<DataFrame>>，支持超时、取消与 EOF |
| DTMF、转写 | LocalParticipant::publish_dtmf / publish_transcription | void；接收通过 RoomObserver |

RPC 处理函数是 `RpcResult(RpcInvocation)`，直接返回 payload 或 RpcError。它不接收完成回调。请求超过响应期限、取消、对端离开或连接丢失都会结束本地等待。

连接成功后，从 Room 或 MediaEngine 获取 LocalParticipant 即可注册和调用服务：

```cpp
auto local = engine.local_participant(); // engine 是已连接的 Room 或 MediaEngine
local.register_rpc("echo", [](client::RpcInvocation request) {
  return client::RpcResult{std::move(request.payload), {}};
});
local.register_stream(true, "chat", [](auto reader) {
  auto text = reader->read_all_text();
  if (!text) {
    LOG_ERROR("Chat", "stream read failed: " << text.error().message);
    reader->close();
    return;
  }
  // 在这里处理收到的完整文本。
});
client::StreamOptions stream;
stream.topic = "chat";
stream.destination_identities = {"peer"};
auto sent = local.send_text("hello", stream);
if (!sent) {
  LOG_ERROR("Chat", "send_text failed: " << sent.error().message);
}
```

StreamHandler 返回 void；读写失败通过相应 Result 或 writer.status() 检查。register/unregister 不构成已执行处理函数的退出屏障；捕获的应用状态应保持有效至 handler 返回，或通过 Room::close 等待退出。

流处理函数接收拥有自身状态的 StreamReader，在独立、有界工作线程执行。可以在该线程读取，也可以把 reader 交给应用线程；读取失败应退出。RPC handler 必须遵守 invocation.deadline/stop。Room::close 会取消请求、关闭 reader 并等待处理函数退出；SDK 无法强制中断任意应用函数，阻塞且不响应取消的 handler 会延迟 close 返回。处理函数不得关闭或销毁所属 Room/MediaEngine，销毁必须由外部所有者线程完成。

同步控制不得在 RoomObserver 或原生传输线程调用。数据服务等待不会占用 RTC 控制线程；该线程继续处理信令、数据通道、超时与重连。内部使用有界队列协调跨线程工作，公开操作不返回 future。

## E2EE

```cpp
auto keys = std::make_shared<client::ExternalKeyProvider>();
keys->set_key(0, key_bytes); // 16 字节 AES-128 密钥；也可 set_passphrase
client::ConnectionOptions options;
options.encryption.keys = keys;
options.encryption.data = true;
options.encryption.media = true;
room.connect(options);
```

数据包使用协议 EncryptedPacketPayload；RPC、流和普通用户数据都经过同一加解密入口。数据轨使用 `_data_track` 的加密分片格式。媒体在 RTP 收发边界进行加解密，仍然只向 Room 输入、输出已编码数据，不做采集或实际编解码。

媒体支持 Opus、VP8、H264、H265。接收端验证 GCM 认证标签，缺失或错误密钥不返回解密内容。默认 require_encrypted=true 拒收应加密的明文。DTMF 和转写没有对应的加密协议载荷，保持协议原有的非 E2EE 消息语义。当前 SFU 只转发 Agent 身份发布的转写；普通用户的本地提交成功不代表服务端转发。

set_key/set_passphrase 会选中新密钥索引，旧索引保留供在途帧解密；确认不再需要后调用 remove_key。密钥由应用分发，不经 SFU 传输。本接口提供外部共享密钥及轮换，不声称实现 Rust 的全部参与者密钥派生/自动 ratchet API。

RoomObserver 增加 encryption_changed、data_error、data_tracks_changed、dtmf_received 和 transcription_received。加密媒体状态按变化报告；错误事件不包含密钥或明文。监听者的生命周期和清除屏障继续遵循 Room 规则。

## 连接与恢复

ConnectionOptions::token_provider 同步返回 Result<string>。初次连接和认证拒绝后的单次刷新分别使用 initial、authentication_failed 原因；服务端 refresh_token 继续用于普通重连。provider 运行在连接工作线程，需要遵守请求 deadline/stop，不能重入 Room 控制。

ConnectionOptions::regions 支持 LiveKit Cloud 的 HTTPS 区域发现、明确配置的 fallback_endpoints 和服务端区域重定向。跨端点携带令牌必须通过 trusted_endpoints 或 Cloud 域名策略校验，不允许 TLS 降级，并限制重定向次数。region endpoint_timeout 只限制信令连接至 JoinResponse 阶段，后续 PeerConnection 连接仍受整个 connect deadline 限制。

连接中断会终止在途 RPC 和流；重连后保留方法与 topic 注册。数据轨重新入会会重发发布请求，旧 generation 的本地句柄失效，需要从 data_tracks() 重新获取。关闭连接会取消待确认发布、订阅和读取，不等待它们自然超时。

自定义媒体图属于 MediaEngine，见 [自定义媒体图](media-engine.md#自定义媒体图)，不依赖旧 RtcGraphApi。共享采集句柄未提供。

## 自动化验证入口

- livekit_client_services：协议、限额、同步返回、关闭取消、AES-GCM 已知答案、密钥轮换和区域信任策略。
- livekit_client_services_integration：本机 SFU 下的双端 RPC、流、数据轨、E2EE、媒体帧还原、Observer、TokenProvider 认证刷新与端点切换。
- 集成测试需要 LIVEKIT_URL、LIVEKIT_API_KEY、LIVEKIT_API_SECRET；没有配置时明确跳过。

自动化使用固定编码帧和隔离 SFU，不代表真实设备人工验收或 LiveKit Cloud 实机验收。

### 2026-11-03 验证记录

- Windows x64 Release SDK 全量编译通过；33 项 CTest 中 31 项通过，2 项 SFU 集成测试因默认环境未设置而跳过，随后使用隔离 SFU 单独执行通过。
- 非原生 Client 配置编译及 7 项 CTest 全部通过。应用当前 Qt6 配置编译通过；24 项 CTest 中 22 项通过，remote_media、studio_media_session 因未配置外部媒体环境跳过。独立 Player 桌面目标不在本次应用构建配置中。
- 服务集成同时验证明文/E2EE：RPC 响应与远端错误、长 UTF-8 文本、Unicode 文件路径与文件流、60KB 数据轨分片、撤销发布、Observer、密钥轮换及错误密钥拒收、VP8/Opus 编码帧逐字节还原、认证刷新和可信端点切换。
- Room 重启恢复回归通过：SFU 重启后重新入会，74 帧视频编码/解码数量一致，红蓝画面内容正确，Opus 解码通过。
- 单元测试覆盖 RPC 超时/取消、关闭等待处理函数退出、旧数据轨句柄失效与重发发布、AES-GCM 已知答案、Opus/VP8/H264/H265 加解密、区域信任边界。H264/H265 目前仅有加解密单元验证，没有本轮原生 SFU 验证。
- 无异常约束检查覆盖 264 个项目 C++ 文件；Room 依赖边界与旧引擎移除检查通过。

完整 LiveKit Cloud 区域发现、Rust 跨语言互通和真实设备人工验收未在本轮执行；本机 SFU 测试不代替这些验证。
