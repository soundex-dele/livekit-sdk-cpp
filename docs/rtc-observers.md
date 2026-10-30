# RTC observer 异步控制

RtcEngine 与 RoomEngine 的网络、轨道控制接口只返回 `ErrorCode` 受理状态。
`ok` 表示已入队，实际结果由预先注册的 observer 通知；不使用 RequestId，也不向操作传入完成回调。
这替代此前的同步控制契约，是一次公共 API 变更。

## 职责

| observer | 通知 |
| --- | --- |
| `rtc::RoomObserver` | 加入、离开、关闭、参与者更新、订阅权限设置结果；连接、远端发现等房间事件 |
| `rtc::PublicationObserver` | RoomEngine 发布结果（PublishedTrack）、取消发布、静音结果及本地发布事件 |
| `rtc::SubscriptionObserver` | RoomEngine 订阅结果（SubscribedTrack）、取消订阅、接收需求结果及本地订阅事件 |
| `rtc::engine::PublicationObserver` | RtcEngine 编码/媒体发布结果、共享输入创建、取消发布、静音、发布事件及发布图故障 |
| `rtc::engine::SubscriptionObserver` | RtcEngine 编码/媒体订阅结果、取消订阅、接收需求、订阅事件及订阅图故障 |

上层可分别实现这些接口，使用 `set_room_observer`、`set_publication_observer`、
`set_subscription_observer` 注册。engine 保存 weak_ptr，上层持有 shared_ptr。
RoomView 的参与者/权限控制也通过 RoomObserver 完成。GraphApi 使用同一组发布/订阅 observer。
本地发布与订阅可使用相同 TrackId，事件中的 publication 字段及独立 observer 区分角色。

## 操作契约

- 操作立即返回，不等待网络或正在执行的控制操作锁。缺少所需 observer 返回 invalid_state。
- 同一对象、同类操作在完成通知开始派发之前再次提交返回 busy。
  publish、publish_media、publish_video、publish_audio 和 GraphApi 发布共享同一发布标识。
- 受理失败不产生完成通知。已受理操作产生一次完成通知；observer 已销毁时安全丢弃通知。
  替换 observer 不转移旧请求的完成结果。替换时已经开始的调用可以继续执行。
- 所有 observer 调用位于房间的串行通知线程，不在持锁的调用栈中执行。
  可以提交新的异步请求，不得等待控制完成、销毁 engine 或抛异常。
  UI 更新由应用转发到自己的 UI 线程。回调参数引用仅在调用期间有效。
- `OperationResult` 带 ErrorCode 和结构化 FailureInfo；创建操作使用 Result<T>。
  发布完成意味着端点与图已经准备好，不能把受理成功当作媒体就绪。
- 控制队列上限 128 个未完成通知的请求。容量用尽返回 resource_exhausted。
  leave/close 另有一个保留槽，关闭时不会因普通请求占满而无法退出。
- leave/close 中断当前协议等待，取消尚未执行的请求，然后清理轨道并离开。
  此期间的新请求返回 busy；close 一经受理，后续控制返回 session_closed。
  已发出的控制可能报告 outcome_unknown，不能声称远端操作已撤销。
- leave 完成后允许重新 join。close 是终态，重复 close 返回 session_closed，不产生第二个完成通知。
- 析构仍是同步资源回收屏障：取消剩余工作、等待控制线程和通知退出。
  析构前上层须停止并发调用；不要在 UI 关键路径依赖析构替代 close。
- 当前实现串行执行协议操作，网络操作的超时预算从开始执行计算；排队时间不计入该预算。
  快照、反馈与 write 维持即时接口。媒体模块初始化保持在 join 前同步完成。

## 实现与迁移

两个 engine 共用房间的有界 AsyncControl 队列。内部保留协议状态机、epoch/incarnation 检查和
同步私有控制方法；控制 worker 执行协议等待，独立通知队列派发 observer。
GraphEngine 媒体流水线不变。leave/close 通过独立的中断标记与 wakeup 唤醒等待，
不会排在一个无法中断的网络等待之后。

`examples/automatic_session/main.cpp` 展示分别实现房间和发布 observer 的调用方法。
现有 CLI、后台会话循环和老测试通过非安装的 `support/rtc_blocking_bridge.hpp` 消费 observer
结果，以保留它们原本的顺序工作流；它不是 SDK 公共 API，也不应从 UI、媒体或通知线程调用。
新增功能应直接实现对应职责的 observer。

验证使用编译、无窗口契约测试；实际网络与设备验收须单独记录。

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
