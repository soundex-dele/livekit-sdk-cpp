# RTC 房间状态机与执行边界

本文按 2026-11-02 的 [RoomEngine 声明](../include/livekit/rtc/room_engine.hpp)、
[实现](../src/backends/rtc/room_engine.cpp) 和 [执行队列](../src/backends/rtc/execution.hpp)
整理。早期“无线程内部状态机 + 外部驱动”的方案已不适用于当前实现。

## 公开边界与所有权

`RoomEngine` 是可独立使用的公共类，通过 `create_livekit_room()` 创建 LiveKit 实例，
也可注入 TransportFactory 做后端测试。独立使用时直接返回 PublishedTrack / SubscribedTrack
编码端点，不创建 GraphEngine、编解码器或设备。

普通媒体应用使用 `RtcEngine`；它独占一个 RoomEngine，并增加 lifecycle 队列、媒体构图器、
资源及发布/订阅图目录。两个类均直接持有状态，没有 PImpl。当前 RtcEngine 没有 room()
或 RoomView；房间快照、服务、成员更新与权限直接通过 RtcEngine 对应方法访问。

TransportFactory 使用内部 RoomTransport 抽象，属于后端注入扩展，不是普通媒体配置要求。
自定义节点走显式 RtcGraphApi；应用不应另建 RoomEngine 去控制 RtcEngine 的同一组媒体图。

## 执行资源

| 执行位置 | 所有者 | 当前职责 |
| --- | --- | --- |
| control | RoomEngine 的 RoomExecution | 房间状态、Transport 请求/事件、端点发送接收、结果匹配 |
| notification | 同一个 RoomExecution，由 RtcEngine 共享 | RoomEngine / RtcEngine 的 Completion、Observer 与内部续接 |
| lifecycle | RtcEngine | 图目录、构造、启动、停止和媒体资源释放 |

SerialQueue 是现有 TaskQueue 的 post / invoke / fence 适配器，没有额外的 RTC 定时调度器、
终态保留槽或恢复状态机。底层网络、编解码、输入输出节点保留自身线程，不属于上述三个队列。

join、publish、subscribe、撤销及成员/轨道控制先入队，在 control 处理；RtcEngine 的媒体
编排先进入 lifecycle，再发起房间操作。snapshot/services 使用 control.invoke 同步取值。
应用无需也不能调用公开 poll() 驱动引擎。Transport 事件主动投递到 control，不由 UI 轮询。

RoomEngine 的结果与持续事件均投递 notification；内部 RtcEngine 续接由该队列再转到
lifecycle。它与用户通知共用队列，慢用户回调可能推迟内部续接。媒体输出回调和
RoomServices 的服务执行器另有契约，不应统称为 notification 回调。

## 操作结果与持续事件

控制方法返回 void，逐次 Completion<T> 接收 Result<T>；持续事件由 RoomObserver /
RtcObserver 的 on_room_event 接收。参数错误与后端失败也使用 Completion。空 Completion
表示忽略本次结果，不能改为依赖持续事件找回操作结果。

- join 成功返回 RoomSnapshot；处理事件批次时先安装其中的 session，完成时可读取初始成员。
- RoomEngine 的发布/订阅成功返回已 ready 的端点；RtcEngine 还要构图并启动后才交付结果。
- unpublish/unsubscribe 在本地原生端点解绑完成时结算；媒体门面先停止对应图。
- 静音、接收需求与权限成功表示本地应用/信令提交，不表示服务端业务确认；成员更新等待协议结果。
- 协议控制仍用请求序号、epoch 和 BindingToken 匹配结果，操作完成与持续状态通知职责不同。

RoomEngine / RtcEngine 的这些控制方法不接受逐次 ControlOptions。不能从房间配置存在
operation_timeout 推导出所有门面操作都支持相同的取消/超时保证；具体协议期限由后端决定。
低层端点方法保留同步 ErrorCode 返回，但静音、需求和 close 只提交操作；ControlOptions
参数当前未使用，不能等同于门面 Completion 的最终结果或逐次取消/超时能力。

## 生命周期与客户端约束

leave 是同步外部拥有者操作。RoomEngine 暂停准入、取消待完成请求、终止并清空轨道，移出并
停止 Transport，设置 disconnected、清空 session，排空 notification 后恢复准入。
RtcEngine 先在 lifecycle 停止和清理媒体图，再调用 RoomEngine::leave 并排空剩余工作。
RtcEngine::close 是 leave 的别名，正常返回后允许再次 join；下一次 join 创建新的 Transport。

应用先停止自身并发请求与写入，再退会/销毁。Completion 与 Observer 可提交后续异步操作，
但不能等待其他回调、同步退会、替换 Observer 或销毁引擎。在 SDK 回调中调用 leave 通过
Completion 报 invalid_thread，不执行退会。Observer 是借用对象，须活到解除注册返回；
解除注册不会取消已接受的 Completion，后者捕获的对象也必须保持有效。

原始媒体输出需由应用交给 UI 所在线程，关闭会等待活动媒体回调返回。保留快照与媒体句柄
不代表已退出的图仍运行。重新入会后重新取得 services 并注册当前会话的处理器。

RtcEngine 当前只在 track_removed 等明确路径移除图条目；没有独立失败图收集器或媒体图
自动恢复状态机。后端轨道事件、媒体节点失败与应用业务重试不能视为同一个自动处理流程。

当前接口示例见 [媒体 API](rtc-media-engine.md)、[编码 API](rtc-encoded-api.md) 和
[Completion / Observer](rtc-simple-observer.md)。本次为源码与文档核对，未运行构建或设备验收。

## 历史验证（2026-10-30）

以下仅记录当时版本，不作为当前实现的验证结果。

- Windows Release 编译通过：状态机、后端、流水线、公共头文件、媒体架构、媒体引擎、
  RtcEngine/LiveKit 集成程序及 room_client 示例。
- 无窗口 CTest 6/6 通过：room_state、rtc_backend、rtc_pipelines、rtc_dependency_boundaries、
  rtc_architecture、rtc_media_engine。
- 集成程序仅完成编译，本轮没有运行真实 SFU 集成，也没有进行实际设备人工验收。
