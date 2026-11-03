> 历史文档：旧 RtcEngine / RoomEngine 及底层实现已移除。本文仅记录当时设计或验证，不代表当前 API；参见 [迁移说明](legacy-engine-removal.md)。

# RoomEngine 与 RtcEngine 的组合边界

RoomEngine 是独立的房间、信令和编码帧传输内核；RtcEngine 在其上编排媒体图。
不启用媒体层也可以使用 RoomEngine 发布/订阅编码帧、管理成员与权限以及使用 RoomServices。

## 构建依赖

- `LiveKit::RTCBackend`：RoomEngine、轨道端点和后端接口，仅依赖线程与任务队列。
- `LiveKit::RTCLiveKit`：LiveKit 协议、信令和 WebRTC 传输；提供 `create_livekit_room()`。
- `LiveKit::RTCEngine`：在房间端点之上提供 GraphEngine 流水线。
- `LiveKit::RTCMediaEngine`：可选采集、编解码、同步和播放能力。

只构建信令和传输时，关闭 `LIVEKIT_BUILD_RTC_GRAPH_NODES`、`LIVEKIT_BUILD_RTC_MEDIA`，
以及需要媒体层的 CLI、Demo、Examples、Player、Studio；保留 `LIVEKIT_BUILD_RTC_NATIVE`。
仅验证房间内核和自定义后端时，也可以关闭 RTC_NATIVE 与 SERVER_SDK。

公共入口不要求包含 `src/` 下的工厂头文件：

```cpp
#include "livekit/rtc/room_engine.hpp"
#include "livekit/rtc/livekit.hpp"

auto room = livekit::sdk::rtc::create_livekit_room();
// 使用 room->join(options, completion)；join 完成后发布或订阅编码帧端点。
```

## 所有权

RoomEngine 支持独立使用；RtcEngine 始终独占内部的 RoomEngine。二者复用同一套
房间实现，但不共享房间实例或连接。

- 只需要信令和传输的模块创建自己的 `RoomEngine` 或调用 `create_livekit_room()`。
- 需要媒体图的模块创建 `RtcEngine(TransportFactory)` 或调用 `create_livekit_engine()`。
- RtcEngine 的 join/leave/close 管理整个内部房间；不提供借用外部 RoomEngine 的构造函数。

因此不需要轨道所有者标记、按所有者释放端点，也不需要多个观察者订阅。
每个实例的轨道、请求和关闭过程都由该实例管理。

## 执行与事件

RoomEngine 只创建 `rtc.control` 和 `rtc.notification`，不创建媒体线程、媒体图或设备。
RtcEngine 另建 `rtc.lifecycle`，使用内部房间的 control/notification。

普通请求（join、发布/订阅、轨道控制和房间控制）使用 `post`，调用返回不等待工作线程，
也不表示操作成功。参数/状态校验失败和后端结果都通过 Completion 返回。
同步查询、同步媒体配置、观察者替换，以及 leave/close/析构仍保留必要的 invoke/fence。

同一队列按入队顺序处理请求；按 ID 订阅的查询和提交在同一个 control 任务完成。
RtcEngine 的媒体请求走 lifecycle，房间和轨道控制直接走 control，避免慢构图阻塞静音等操作。
跨队列没有隐含的先后保证：publish/subscribe 成功后的控制应从其 Completion 发起，
不能用“方法已经返回”或一次 snapshot 查询代替完成通知。join 后的操作也应等待 join 成功。

所有公共 RoomEngine Completion 和 Observer 始终在 notification 执行。
RtcEngine 的媒体步骤在 lifecycle 执行，最终 Completion 仍在 notification。
回调允许提交普通请求，不允许等待 SDK 回调、同步 leave、替换观察者或销毁引擎。
构造、销毁和这些同步生命周期操作由外部所有者线程执行。

RoomEngine 保留单一 `set_observer()` 槽。RtcEngine 在内部注册自己，接收房间事件，
完成媒体清理后通过自己的 RtcObserver 转发给应用。独立使用 RoomEngine 时由模块直接
注册 RoomObserver。观察者必须存活到替换或 `set_observer(nullptr)` 返回；替换会等待
已排队的旧观察者回调结束。

RtcEngine 离会时先停止媒体图，再关闭内部房间并排空媒体续处理与通知。
离会先关闭请求入口：已接受但尚未执行的请求返回 cancelled，关闭期间提交的新请求返回
session_closed；已交给后端的未完成请求由房间关闭流程结算。离会完成后可以重新 join
再发布/订阅。请求入口的互斥锁仅保护检查与入队，不跨任务执行或等待。

## 发布结果与失败回滚

RoomEngine 发布完成表示传输端点就绪，结果为 `PublishedTrack` / `SubscribedTrack`。
RtcEngine 发布完成还要求媒体图启动，结果为 `MediaPublication` / `MediaSubscription`。
这是两个不同层次的完成条件，而不是同一个完成状态的重复维护。

媒体图构建或启动失败时，RtcEngine 先停止部分图，再请求解绑对应的传输端点。
最终失败回调在解绑结果返回后交付，保留原始构图失败。若解绑也失败，追加该失败详情，
保留失败端点以便显式 unpublish/unsubscribe 重试；关闭整个引擎也会清理房间。

## 验证

`rtc_backend` 验证独立房间、异步入队、订阅顺序、关闭期间请求拒绝和观察者替换屏障；
`rtc_pipelines` 验证媒体线程、慢构图期间的非阻塞提交与房间控制、
待完成请求取消、延迟解绑与失败回滚；`public_room` 仅链接 RTCLiveKit 并使用公共头文件。
`rtc_dependency_boundaries` 在关闭媒体图构建时仍运行。
这些是无窗口自动化检查，不代表实际摄像头、麦克风或播放设备人工验收。
