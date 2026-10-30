# RTC 完成回调重构（2026-10-30）

## 对外接口

RtcEngine、RoomEngine、RoomView 和 RtcGraphApi 的单次控制现在传入 Completion<T>，同步返回
ErrorCode 仅表示是否受理。无值结果采用 Result<void>。普通操作要求非空回调；leave/close
可以省略回调，不再要求注册 Observer。没有新增 OperationHandle、RequestId 或逐请求取消 API。

RoomObserver、PublicationObserver 和 SubscriptionObserver 只保留持续事件、图故障及自动
codec 替换等通知。已受理 completion 独立持有自己的捕获对象，不受 Observer 替换或销毁影响。
公共头文件断言、自动会话示例、CLI/应用共用阻塞桥接和契约测试已同步迁移。

迁移时，将原来 Observer 中的单次完成处理移到调用参数中。例如：

```cpp
auto admitted = engine.join(options, [](Result<void> result) {
  if (!result) {
    report_failure(result.error());
    return;
  }
  // 入房完成；可以在这里提交下一次异步操作。
});
if (admitted != ErrorCode::ok) {
  report_admission_failure(admitted);  // 未受理，不会再调用 completion。
}
```

示例中的 report_* 由应用实现。同步工厂、快照和媒体 write 保持即时接口；原有
ControlOptions 放在 completion 参数之后。这是控制接口的源码兼容性变更，调用方需要迁移。

## 执行与生命周期

- 保留已有 RoomEngine 的按轨道异步协议调度和同轨顺序。RoomEngine 与 RtcEngine 的不同轨道
  可以同时等待 ACK 和原生解绑；join/leave/close 仍是房间屏障。
- RtcEngine 的普通发布/订阅分成端点确认和建图阶段；媒体生命周期、图停止及维护使用独立
  生命周期线程，公共控制 worker 不等待这些操作。解绑结果返回前保留已退出注册表的图资源。
- Completion 和 Observer 共用串行通知线程。不内联调用、不持状态锁调用用户代码。
  支持回调内提交下一步及异步关闭；阻塞桥接在 SDK 回调/工作线程直接拒绝等待。
- RoomEngine 的资源停止不再在控制锁中等待通知线程。关闭终态通知之前消费既有事件尾部。
- 析构先结算控制并排空通知，再停止生命周期执行器，等待已结算任务释放捕获对象，最后释放
  图、媒体和房间。应用须先停止并发调用，并在 SDK 回调之外析构。
- 修正订阅对已安装媒体模块的跨线程观察，使用原子发布标记。

## 保留约束

同类同轨操作在通知开始前仍返回 busy。超时继续使用房间默认值及既有 ControlOptions，
保留从执行开始计时的语义。自动 codec 策略、构图及失败回滚在生命周期线程串行执行，
其中的同步后端等待仍可能延迟媒体生命周期工作；没有宣称所有设备操作都可抢占。

慢通知仍会推迟完成回调；媒体回调必须最终返回。不能在任何 SDK 回调中等待关闭或析构引擎。
极端通知投递失败会记录故障并释放容量/捕获，不会切换到控制线程补发用户回调。

Android 尚无构建模块，本次更新的是 JNI 完成回调、GlobalRef、Executor 和异步 release 的绑定
说明，不是已实现或已验收的 Android 绑定。详见 [JNI 说明](android-jni-results.md)。

## 验证

- SDK Windows Release 全量编译通过，包含公共头文件检查、示例、CLI 和集成客户端。
- 会议、远控、投屏、直播四个应用 Release 编译通过。
- 最终相关 CTest 11/11 通过：result、async_result、rtc_backend、rtc_pipelines、
  rtc_dependency_boundaries、rtc_architecture、rtc_media_engine、rtc_codec_policy、
  rtc_native、rtc_session、rtc_services。
- 契约回归覆盖无需 Observer、单次结果、回调重入关闭、回调捕获释放、慢通知与资源停止、
  SDK 回调内拒绝阻塞等待，以及两种 Engine 的跨轨 ACK/解绑顺序。
- 本地隔离 SFU 三类集成均取得通过结果，结果目录位于 SDK 下：
  - RoomEngine 多轨及控制：`out/rtc-livekit-integration/20261030-170156-jrut9p1c`。
  - RtcEngine 编码多图：`out/rtc-livekit-integration/20261030-165845-5p9_pizh`。
  - RtcEngine 合成音视频：`out/rtc-livekit-integration/20261030-165845-4o7eyhww`。
  合成媒体报告中的旧字段 synchronousControl 指应用阻塞适配器，不代表 SDK 控制接口同步执行。

并发运行三类 SFU 集成时，RoomEngine 曾在测试第 471 行取消静音后的传输阶段超时，视频轨道 2
未收到帧，其余三轨收到帧；失败记录保留在 `out/rtc-livekit-integration/20261030-165845-pvgxypk9`。
随后使用同一个客户端二进制单独复跑通过，未修改测试或延长超时。该现象未复现，原因尚未确定，
不能据此认定稳定性问题已修复。

以上使用编译、无窗口测试和隔离本地 SFU；未操作用户桌面，未进行真实摄像头、麦克风、
扬声器或设备人工验收。

最终日志位于聚合仓库 `out/`：`rtc-completion-sdk-final-check.log`、
`rtc-completion-app-final-build.log`、`rtc-completion-sdk-final-tests.log`、
`rtc-completion-room-isolated-integration.log`、`rtc-completion-engine-final-integration.log`、
`rtc-completion-media-final-integration.log`。接口契约见
[Completion 与 Observer](rtc-observers.md)。
