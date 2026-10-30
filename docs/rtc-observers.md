# RTC 单次完成回调与持续 Observer

RtcEngine、RoomView 和 RtcGraphApi 的异步控制立即返回 `ErrorCode` 受理状态。
每次调用传入 `Completion<T>`（`std::function<void(Result<T>)>`），普通无值控制使用
`Completion<void>`。Observer 只接收持续状态变化。不提供 OperationHandle、RequestId 或新增的逐请求取消 API。

## 调用契约

```cpp
auto code = engine.join(options, [&engine, spec, on_published](Result<void> joined) {
  if (!joined) {
    report_failure(joined.error());
    return;
  }
  // on_published 接收 Result<MediaPublication>，在同一个串行通知线程执行。
  auto admitted = engine.publish_media(spec, on_published);
  if (admitted != ErrorCode::ok) report_admission_failure(admitted);
});
if (code != ErrorCode::ok) report_admission_failure(code);
```

- `ok` 仅表示已受理。受理失败不调用 completion；已受理操作在结算后调用一次。
- 普通操作必须提供非空 completion，否则返回 `invalid_argument`；leave/close 可以省略。
- 无需注册 Observer 就能操作和关闭。SDK 持有已受理 completion，直到派发结束或异常清理；
  Observer 替换、销毁不会改变本次回调。捕获的业务对象须有有效期保证，避免捕获引擎强引用形成环。
- 同对象、同类未完成通知的请求返回 `busy`。所有发布入口共享同一轨道的发布标识。
- 单次结果使用 Result<T>/Result<void>，完整保留 FailureInfo。同步工厂、快照与 write 保持即时接口。
- 媒体发布成功表示端点就绪且图已经启动，不代表远端首帧已显示。后续图故障由 Observer 通知。
- 参与者更新成功要求服务器 ACK；权限和接收需求设置遵循各接口已有的信令提交契约。
- 上限为 128 个未完成通知的普通请求，容量保留至通知结束；leave/close 另有一个保留槽。
  通知开始时释放同类请求标识，允许在回调中提交下一步；容量满时仍须处理受理失败。
- 极端内存/通知队列故障可能导致通知无法投递。RAII 仍释放捕获对象及容量并记录故障，
  绝不在控制线程内联补发用户回调。阻塞适配器会将丢弃的完成任务识别为 callback_failed。

## 持续通知

| Observer | 职责 |
| --- | --- |
| RoomObserver | 连接、重连、远端发现、成员及房间状态、编解码策略变化 |
| PublicationObserver | 发布状态变化；RtcEngine 额外提供图故障及自动编解码替换通知 |
| SubscriptionObserver | 订阅状态变化；RtcEngine 额外提供图故障 |

通过 set_*_observer 注册 weak_ptr，应用持有 shared_ptr。完成通知不再是 Observer 虚函数。
自动 codec 替换没有应用侧单次调用，其 on_video_codec_republished 仍属于持续通知。
Observer 参数引用只在当前调用有效；需要跨线程使用时复制拥有数据的结果或快照。

## 线程与死锁约束

1. 调用线程只提交，不等待网络、设备或图停止。不保证硬实时安全；音频实时线程须转发控制意图。
2. 有界 AsyncControl 管理同轨道顺序与房间屏障。不同轨道可以同时等待 ACK 或原生解绑。
3. RoomEngine 同步推进协议状态，通过内部结果回调报告完成；RtcEngine 的执行上下文转换为 AsyncResult，控制 worker 只处理已就绪 continuation。RoomEngine 不拥有线程或状态锁，由上层串行驱动。
4. RtcEngine 在独立生命周期序列执行建图、媒体启停和回收。正常发布/订阅将端点等待与建图分开，
   静音、需求和解绑使用异步协议结果；图构建、失败回滚和自动 codec 切换仍在生命周期序列中串行。
   原生设备停止及 codec 策略内部同步协议等待可能延迟该生命周期序列，不占用公共控制 worker。
5. 所有 Completion 和控制类 Observer 在房间的同一个串行通知线程运行，SDK 不持状态锁调用它们。
   媒体帧回调继续使用独立数据调度线程。
6. 回调允许读取快照、提交操作、异步 leave/close。禁止等待 SDK 工作、同步销毁 Engine、
   或与 UI 线程相互同步等待。回调不得抛异常；边界会捕获异常并记录通知故障，不重试回调。
7. 回调不会内联到提交调用栈，但另一线程上的回调可能先于提交函数返回。提交前准备好回调上下文。
8. 阻塞适配器使用 is_callback_thread() 与媒体回调保护，在 SDK 控制、生命周期或通知上下文直接
   返回 invalid_thread。应用自己创建的 future/锁仍须遵守禁止阻塞的约定。

## 退出和释放

leave/close 取消排队操作，中断活跃协议等待，再停止图和传输。已发出的远端控制可能返回
outcome_unknown，不能当作远端撤销。超时继续使用房间默认值及已有 ControlOptions，不新增配置体系；
既有协议超时从开始执行计算，通知排队不改变已完成结果。

leave 完成后允许 join。close 受理后进入终态，后续请求和重复 close 返回 session_closed。
关闭不依赖 Observer，也不依赖应用完成回调来推进资源清理。最终通知在先前完成任务之后派发，
并在通知线程清理既有事件尾部；不会在控制锁内等待通知线程。慢通知会推迟关闭通知的交付。

析构仍是同步屏障，应用先停止并发调用，再由非 SDK 回调线程销毁引擎。不要在 close 回调内直接
reset/delete；向所有者线程投递释放任务。已转发到 UI/Java Executor 的任务不属于原生关闭屏障。

## 迁移

- 将 on_join_completed、on_publish_completed 等 Observer 方法改为对应调用的 completion。
- RoomView 的参与者/权限方法同样传入 completion；GraphApi 在图节点/factory 参数后传入 completion。
- 旧控制选项仍放在 completion 后，例如 set_muted(id, true, on_done, options)。
- CLI、后台 Session 和历史测试使用非安装的 support/rtc_blocking_bridge.hpp 保留顺序流程。
  它直接消费每次调用的回调，不再按 Observer/TrackId 建立结果映射。
- examples/automatic_session 展示持续 Observer、单次回调与应用主线程等待；JNI 见
  [Android 绑定说明](android-jni-results.md)。当前仓库没有 Android 绑定构建目标。

## 历史验证记录

以下记录属于此前 Observer-only 及按轨道调度版本，不代表本次 Completion 重构的验证结果。
本次结果另见 [Completion 重构记录](rtc-completion-refactor-20261030.md)。

## 本次验证（2026-10-30）

- SDK Windows Release 全量编译通过，包含 CLI、示例和集成客户端。
- 会议、远控、投屏、直播四个桌面应用 Release 编译通过。
- 7 项无窗口 CTest 通过：result、async_result、rtc_backend、rtc_pipelines、
  rtc_dependency_boundaries、rtc_architecture、rtc_media_engine。
- 新增直接 observer 测试覆盖延迟网络等待、重复请求、回调重入提交、取消、
  有界队列、关闭保留容量、析构排空、observer 替换/销毁和结构化错误。
- 本地隔离 Go SFU 的三组集成通过：RoomEngine 原生多轨/控制、RtcEngine 编码多图、
  RtcEngine 合成音视频。结果目录分别为 SDK 下：
  `out/rtc-livekit-integration/20261030-142706-xfj18pi1`、
  `out/rtc-livekit-integration/20261030-142156-7eq9oghh`、
  `out/rtc-livekit-integration/20261030-142249-pw1vg3l9`。
  合成媒体客户端报告中的旧字段 synchronousControl 指应用层等待适配器，SDK 已使用 observer。
- 构建日志位于聚合仓库 `out/rtc-observer-build-final.log`、
  `out/rtc-observer-app-build.log`；补充契约测试构建见 `out/rtc-observer-final-tests-build.log`。

以上是自动化编译、无窗口和合成媒体验证；未进行真实摄像头、麦克风、扬声器或设备人工验收。

## 通知与自动回收优化复验（2026-10-30）

- 后端独立配置的后端、流水线测试目标编译通过，相关 CTest 3/3 通过。
- Windows Release 下重新编译相关测试目标，最终 CTest 5/5 通过：`rtc_backend`、
  `rtc_pipelines`、`rtc_dependency_boundaries`、`rtc_architecture`、`rtc_media_engine`。
- 新增回归覆盖投递抛异常、通知任务丢弃、observer 抛异常、同标识复用、满载时合并
  回收请求、发布/订阅图自动回收、服务端撤销及等待原生解绑期间析构退出。
- 取消测试使用较长协议超时，同时保留两秒取消完成断言，避免构建负载使协议超时
  抢先发生；媒体测试补齐回调重入及退房重入的阶段标签。
- 媒体回归曾在阻塞适配器的回调重入用例超时。使用当前工作区的媒体回调保护后，
  该用例及最终五项回归全部通过。
- 构建日志位于聚合仓库 `out/rtc-engine-optimization-core-build.log`、
  `out/rtc-engine-optimization-build.log`、`out/rtc-engine-optimization-media-build.log`
  和 `out/rtc-engine-optimization-final-tests-build.log`。

该轮没有重跑真实 SFU 集成或实际设备人工验收。当时协议操作仍串行执行；后续进展见下节。

## 按轨道调度改造（进行中，2026-10-30）

已接入 RoomEngine 的发布、订阅、静音、接收需求、参与者更新、权限提交及原生解绑。
保留同轨顺序、join 屏障、leave/close 取消、通知容量和旧 epoch/incarnation 隔离。
新增用例使用显式扣留的 ACK/解绑回调，验证其它轨道可以先完成，并检查多个活跃等待
在关闭时一起取消。同步端点删除的重复等待不会覆盖前一个等待者。

RtcEngine 构图和回滚、失败图异步回收、房间级编解码策略事务尚未完成迁移；不能将
RoomEngine 的并行验证表述为整个 RtcEngine 已消除队头阻塞。

本阶段验证：

- 后端独立配置相关目标重新编译，CTest 3/3 通过。
- Windows Release 相关目标重新编译及链接，最终 CTest 6/6 通过：`rtc_backend`、
  `rtc_pipelines`、`rtc_dependency_boundaries`、`rtc_architecture`、`rtc_media_engine`、
  `rtc_codec_policy`。
- 覆盖不同轨道延后完成、同轨顺序、join/leave 屏障、并发结构化错误、通知投递失败、
  慢原生解绑、重复删除等待者及原生停止完成前不交付取消结果。
- 编解码策略测试原先在传输收到发布请求后即检查替换图的静音快照，可能读到尚未安装
  的空图。现等待替换轨道 ready 和策略事务结束，再保留原有静音断言。
- 构建日志位于聚合仓库 `out/rtc-deferred-build.log`、`out/rtc-deferred-windows-build.log`、
  `out/rtc-deferred-final-core-build.log` 和 `out/rtc-deferred-final-tests-build.log`。

本阶段未重跑真实 SFU 集成，未进行实际设备人工验收。
