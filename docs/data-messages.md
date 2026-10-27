# DTMF 与结构化转写

`LocalParticipant::publish_dtmf` 和 `publish_transcription` 使用官方 `DataPacket.sip_dtmf` / `DataPacket.transcription`，固定 schema 的 descriptor 合约检查覆盖字段编号和类型。它们不使用自定义 topic，也不涉及 SIP/Agent 管理 API。

```cpp
auto local = room.local_participant();
local.publish_dtmf({11, "#"}).get();
livekit::sdk::Transcription text{
    local.identity(), publication.sid(),
    {{"segment-1", "你好", 100, 200, false, "zh"}}};
local.publish_transcription(text).get();
text.segments.front().text = "你好，世界";
text.segments.front().final = true;
local.publish_transcription(text).get();
```

消息总是使用可靠数据通道。future 成功仅表示本地通道提交，不表示远端收到或服务端允许转发；`wait_for_ack` 不适用于这两个接口。支持 `OperationOptions` 超时/取消，发送后取消可能返回 `outcome_unknown`；断线不重放。没有持久化或断线离线队列。

本地官方服务端 1.13.10 只转发 Agent 身份发布的转写；普通参与者的消息可能被静默丢弃。应用应从业务服务获得有适当权限的 Token，SDK 不自动提升身份。DTMF 验证 RFC 4733 的 0..15 事件及对应的 `0123456789*#ABCD`，不处理电话拨号或服务端 SIP 管理。

通过 `RoomCallback.on_dtmf_received` 和 `on_transcription_received` 接收消息。回调遵循现有 Room 串行线程契约。转写中的 participant_identity 是被转写者，`DataReceiveParams.sender_identity` 是数据发送者，应用应分别判断。每次回调提供片段更新，不累积历史文本；应用按发送者、轨道、segment ID 合并临时与最终结果。时间值按官方 uint64 原样保留，不推断声卡时钟或转换单位。

每条转写最多 64 个片段、合计 12,000 字节 UTF-8 文本，编码 payload 最多 15,000 字节，并保留外层身份字段的空间。非法 UTF-8、反向时间范围、空 segment ID 或缺失轨道/参与者会被拒绝。接收端同样验证，畸形消息不触发应用回调。

## 互通夹具

Go 仅用于独立测试进程，不是 SDK 运行依赖。参考 SDK 工作树保持只读；构建脚本将源码、go.mod 和 go.sum 放到 `out/go-interop-peer`，保留 Go SDK revision、工作树状态和二进制 SHA-256。

```powershell
python tests/go_interop/build.py --sdk D:/workspace/server-sdk-go --go out/toolchains/go1.26.10/go/bin/go.exe --compiler-bin D:/msys64/mingw64/bin --proxy 'https://proxy.golang.org|https://goproxy.cn|https://goproxy.io'
# Go peer 使用 CGO Opus，运行测试进程的 PATH 需要包含 libopus 所在目录。
python tests/go_interop_integration.py --client out/windows-x64/Release/livekit_reconnect_integration.exe --server <livekit-server.exe> --go-peer out/go-interop-peer/go-interop.exe
```

夹具验证 C++ 440 Hz PCM → Go Opus 解码、Go 880 Hz PCM → C++ PCM 读取，以及双向 DTMF、中文/英文转写和 partial/final 更新。C++ 发布使用 `TrackPublishOptions.disable_red = true`，因为当前 Go PCMRemoteTrack 仅接受 Opus。默认 RED 行为保留，禁用选项随已确认轨道的完整重新入会恢复。
