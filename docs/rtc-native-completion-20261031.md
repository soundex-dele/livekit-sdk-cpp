# RTC 原生完成回调（2026-10-31）

本次覆盖 RtcEngine / RoomEngine 的入会、媒体发布订阅、SDP 与统计链路。
RoomServices 的 RPC、数据发送和流接口保留原有 future，另行迁移。

## 单次结果与持续事件

NativePeer 不再同时返回 future 并接收无参 completed 通知：

```cpp
void create_local(bool offer, bool ice_restart, Completion<NativeDescription> completion);
void set_remote(NativeDescription description,
                std::map<std::string, std::string> mid_to_track,
                Completion<void> completion);
void statistics(Completion<TransportStatistics> completion);
```

`Completion<T>` 是 `std::function<void(Result<T>)>`。WebRTC 的操作 Observer 将成功、失败
或取消直接交付给这一个回调。局部校验失败也走同一出口；原子结算标记避免重复完成。
WebRTC 释放尚未完成的 Observer 时，回调收到 cancelled，避免原来 broken_promise 的异常语义。
关闭前已经完成的操作保留原结果，关闭后新请求返回 session_closed。

原生回调可能在 WebRTC 完成线程执行，也可能在本地拒绝时内联执行。LiveKitTransport
通过有界队列将 Result 投递给会话线程，唤醒后由会话线程更新 SDP 状态和统计快照。
原生回调不直接操作协商状态，也不直接调用应用。会话退出时关闭 Peer，销毁队列；迟到回调
通过弱引用检查，不访问已销毁的会话结果槽。

RtcEngine / RoomEngine 的公共完成回调继续使用 notification 队列；连接、轨道和统计等
持续事件继续使用 Observer。control、lifecycle、notification 的分工保持不变。
同步快照与本地设置向既有 TaskQueue 投递任务，用共享等待状态与条件变量同步，
不再经过 packaged_task/future；等待状态保留到通知方退出，避免同步对象先于通知销毁。
NativePeer 测试通过条件变量等待。已有应用/测试阻塞桥接保留在外层，不进入生产引擎链路。

## 显式错误与迁移边界

FailureInfo 等无异常错误类型移入 failure.hpp，Result 不再依赖 SDKError。
RoomTransport 的同步提交、信令连接/发送/接收、令牌请求、区域选择和重连预算采用 Result。
原生 Peer、编码发送器/接收器和 TransportRuntime 的初始化错误通过 status() 查询。

以下目标启用了编译器禁异常选项，并按目标单独构建验证：

- livekit_rtc_backend
- livekit_rtc_native
- livekit_rtc_signaling

尚未完成整个 RTC 媒体依赖链的异常清理。媒体图、设备/编解码实现以及 LiveKitTransport
调用的部分旧实现仍依赖异常；其迁移适配集中在 legacy_control_boundary.hpp。
这些目标未启用全局禁异常选项。RoomServices 的实现和错误契约此次不改。

## 验证

- macOS x86_64 相关目标编译通过，公共 Engine 头文件检查通过；未验证 Windows 编译。
- 最终 CTest 9/9 通过：result、rtc_backend、rtc_signaling_policy、rtc_pipelines、
  rtc_dependency_boundaries、rtc_native、rtc_flexfec、rtc_architecture、rtc_media_engine。
- RoomEngine 回归另外连续运行 10 次通过。此前使用 TaskQueue 旧同步助手时复现了等待超时，
  线程采样显示调用方等待而工作线程已空闲；改用共享等待状态后未再复现。
- 原生测试覆盖正常 SDP/统计、非法 SDP、关闭中的请求及关闭后请求、单次完成。
  媒体引擎测试覆盖旧入会校验错误转为 Result，并在通知线程完成一次。
- 区域发现集成客户端按新 Result 接口更新，并通过语法检查；未运行区域故障注入集成。
- 最终隔离 SFU 集成通过：入会、发布、订阅、离会各 2 次完成回调，4 次持续统计事件，
  33 个音频帧、10 个视频帧，关闭后保留的帧存储仍有效。使用合成音视频与应用输出，
  没有进行摄像头、麦克风或扬声器的实际设备人工验收。

测试日志位于 `out/rtc-callback-verification/`；最终 SFU 报告目录为
`out/rtc-livekit-integration/20261031-230442-_9xdz34n/`。该集成使用 observer-only 模式，
不包含发送端时钟/音视频呈现时钟验收。
