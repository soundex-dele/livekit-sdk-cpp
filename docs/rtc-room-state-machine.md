# RTC 房间状态机与执行边界

`RtcEngine` 是公开入口，直接持有状态并实现业务逻辑，不再使用 PImpl。内部 `RoomEngine` 负责协议状态，
不创建线程或任务队列，不持有协议状态锁，也不检查调用线程身份。
调用方必须串行访问同一实例，包括查询、事件处理、超时和销毁，并保证跨线程交接的同步。

## 操作与事件

RoomEngine 同步发起本地操作，不等待网络结果。每次操作传入一个内部结果回调，
回调本身关联该次请求，因此不需要再由调用方维护一套房间操作 ID 映射。
Transport 需要关联 ACK 时，仍使用原有的协议操作 ID、epoch 和轨道 incarnation。

- 操作结果回调：报告该次请求的最终成功或失败，也包含立即发生的参数错误。
- 房间状态回调：报告连接、远端轨道、权限等持续事件，不承担请求完成职责。

二者均在驱动 RoomEngine 的调用过程中执行，不承诺线程身份。内部接收者不能抛异常，
不能重入修改 RoomEngine；用户 Observer 和 Completion 由 RtcEngine 的通知执行器派发。
协议批次处理完成后才交付结果，确保同批次的 connected 和初始 roster 已全部生效。

完成条件沿用协议契约：参与者更新等待 ACK；权限更新表示信令提交；解绑表示本地
Transport 释放完成。取消已发出的控制请求可能得到 `outcome_unknown`，不能推断服务器未执行。

## 执行资源

`RtcEngine` 拥有 `RtcExecutionContext`。它仅提供通用执行资源，没有 join、publish
等房间业务接口，也没有对应的 async/locked 包装。业务编排直接调用 RoomEngine。

- AsyncControl 负责准入、容量、重复请求、操作排序、屏障和完成交付保留。
- 生命周期执行器处理 GraphEngine 和设备启停。
- I/O 驱动负责唤醒、期限等待和协议轮询。
- 外部访问门串行化协议发起、轮询和查询；RoomEngine 不知道该门的存在。
- 通知执行器隔离用户回调，保留事件容量及事件先于对应完成通知的顺序。

原生回调只写入安全的输入通道并唤醒驱动，不直接进入协议状态。协议组件的 `poll()`
不等待；没有上层驱动调用它，就不会自行处理输入或超时。媒体端点和 Transport 内部
跨线程的数据通道继续保留必要的同步。

`RoomView` 的查询由执行上下文保护；修改请求经过 RtcEngine 的统一调度器。
旧轨道句柄持有引擎控制端口的弱引用，不再指向协议状态。

## 生命周期

关闭先停止准入并中断等待，再保留推进操作所需的协议驱动和通知执行器，直到已接受
操作完成。媒体清理结束后停止协议事件源，解除回调并排空通知，最后释放对象。
原生回调通过弱引用或不含对象指针的信号句柄处理晚到通知。

## API 迁移与验证

删除公共 `livekit/rtc/room_engine.hpp`、`create_livekit_room()` 和接收 RoomEngine 所有权
的 RtcEngine 构造函数。应用使用 `engine.hpp`、`create_livekit_engine()` 和 `RoomView`。
后端测试通过 TransportFactory 注入；独立协议契约的兼容驱动位于非安装的测试辅助代码，
不会成为生产引擎的第二层门面。

`room_state` 直接验证无自动驱动、无固定线程限制、ACK、重复 ACK、取消后晚到结果、
本地拒绝和由调用方驱动的超时。原有 backend、pipeline、media 契约继续覆盖调度与媒体
行为。所有自动化验证均无窗口，不代表实际摄像头、麦克风或扬声器已经人工验收。

### 本次验证（2026-10-30）

- Windows Release 编译通过：状态机、后端、流水线、公共头文件、媒体架构、媒体引擎、
  RtcEngine/LiveKit 集成程序及 room_client 示例。
- 无窗口 CTest 6/6 通过：room_state、rtc_backend、rtc_pipelines、rtc_dependency_boundaries、
  rtc_architecture、rtc_media_engine。
- 集成程序仅完成编译，本轮没有运行真实 SFU 集成，也没有进行实际设备人工验收。
