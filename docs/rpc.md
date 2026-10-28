# 客户端 RPC

`Room::register_rpc_method` / `unregister_rpc_method` 注册处理器；`LocalParticipant::perform_rpc` 返回 `future<string>`。使用官方可靠 DataPacket request/ack/response，版本 1，可与 Go SDK 的 RegisterRpcMethod / PerformRpc 互通。

```cpp
room.register_rpc_method("echo", [](livekit::sdk::RpcInvocation request,
                                    livekit::sdk::RpcCompletion done) {
  done({request.payload, {}});
});
auto result = room.local_participant().perform_rpc({"peer", "echo", "你好"});
// 在控制回调以外的线程等待。
auto reply = result.get();
```

处理器由每个 Room 的独立串行调度线程调用。启动异步工作后应及时返回，应用需自行持有异步资源、观察 `request.stop` 与单调时钟 `request.deadline`。完成函数可从任意线程调用；SDK 只接受第一次有效完成。注册保留到取消注册或 Room 销毁，且跨重连和 Room 重用有效；取消注册不撤销已经派发的调用。

应用可返回 `RpcResult.error` 或抛出 `RpcError`，其 code/message/data 会发送给对方。其他异常统一返回 1500，不发送原始异常文本。调用方必须在业务层验证 `caller_identity` 及参数；注册处理器不自动赋予调用者业务权限。ACK 仅表示接收请求，future 成功表示收到业务响应。

默认总超时 15 秒、ACK 等待最多 7 秒。允许设置 1 毫秒至 5 分钟总超时，截止时间从本地提交开始，包含排队及建通道时间。发给远端的处理预算为 `max(1s, response_timeout - 7s)`，与 Go 的网络缓冲规则一致；Go 自身会将较短调用总超时提升到 8 秒，C++ 不延长用户设置的本地截止时间。

`RpcCallOptions.stop` 在尚未取出发送时返回 `SDKError(cancelled)`；取出发送后返回 `outcome_unknown`，对方可能已执行。取消只停止本地等待，协议没有远程撤销消息。响应迟到不改变已取消/超时的结果。信令恢复开始、完整 rejoin、房间迁移、对方离开或 Room 结束均使待处理调用以 1503 结束，同时取消接收侧处理器；不会重放 RPC，应用若需重试，应设计业务幂等键。

约束：UTF-8 请求/响应各最多 15,360 字节，方法名最多 256 字节，错误 message/data 分别最多 256/15,360 字节。错误文本按 UTF-8 边界截断。每个 Room 最多注册 64 方法、128 个待响应出站调用、128 个入站记录、32 个待启动处理器及 512 个出站包；原生通道缓冲达到 256 KiB 时暂停提取，每轮最多 4 包。容量不足会拒绝新工作，超时仍生效。这些限制不构成网络延迟或吞吐保证。

入站重复请求使用 `(caller_identity, request_id)` 缓存，完成结果最多保留 60 秒；满时先逐出已完成记录。仅在缓存有效期内抑制重复执行，不提供跨重连或缓存逐出后的 exactly-once 保证。未知方法返回 1400，未知版本返回 1404。`RpcCallOptions::compress` 可选择 raw-DEFLATE 请求，接收支持压缩请求和响应，解压后仍执行原始负载上限；无效或超限压缩数据按请求/响应负载错误处理。默认发送不压缩，避免向未支持该字段的旧客户端发送。

RTC 会话为 RPC 和流发送分配各自每轮配额，流之间轮转；这不保证底层可靠 SCTP 没有队头阻塞。文本/字节流和文件入口见 [RTC 客户端扩展](client-extensions.md)。

不得在普通 Room 控制回调中阻塞等待 RPC future。处理器内同步等待其它工作会阻塞该 Room 的后续 RPC 处理器；应使用异步启动模式。Room 销毁不会无限等待违规的阻塞处理器，迟到应用任务可比 Room 活得更久，因此禁止捕获已释放对象。应用 stop 回调也必须及时返回。

验证入口：`tests/rpc_contract.hpp` 覆盖状态机与边界；`tests/go_interop_integration.py` 在本机隔离官方服务器上验证双向 UTF-8 echo、自定义错误及未知方法，同时回归 PCM/DTMF/转写；追加 `--rpc-only` 验证双方无媒体发布时的数据通道建立与 RPC。构建和实测结果见 [进度记录](client-parity-progress.md)。这不是物理设备、跨平台或弱网长期验收。
