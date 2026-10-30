# RTC 同步控制与 Result 重构方案

日期：2026-10-29。版本：锁串行化简化方案。

状态：历史方案。2026-10-30 已进一步统一为无数据操作返回错误码、有值操作返回 Result<T>，当前接口以 [RTC 同步控制接口](rtc-direct-control.md) 为准；下文保留有值结果的早期设计，无数据操作已更新为错误码。本文替换此前“专属控制 TaskQueue + postSyncTask + 抢占取消”的方案。Windows 编译、契约测试与隔离 SFU 集成结果见 [实施验证记录](rtc-sync-result-validation-20261029.md)；真实设备与其他平台尚未人工验收。后文“当前实现”描述重构前基线，目标设计现已落地。

## 1. 最终设计

采用 **调用线程执行 + 操作锁串行化 + 同步返回 Result**。

1. 删除 RTC 的 `AsyncResult`、`CompletionSource` 和 `.then()` 操作编排，不保留兼容层。
2. 公共可失败控制接口同步返回 `Result<T>`，成功携带值，失败携带结构化错误；无数据操作返回 `ErrorCode`，成功为 `ErrorCode::ok`，失败为具体错误码。
3. 不引入控制任务队列，不通过 `postSyncTask()` 转发公共控制调用，也不为控制 API 创建专属线程。移除现有 RtcEngine 控制 TaskQueue 及对应执行器依赖。
4. 同一引擎会话的控制操作通过一把非递归操作锁串行执行。入会和离会互相等待，不抢占、不要求相互打断。
5. 保留现有必要的房间事件、网络、媒体和设备工作线程。等待连接结果期间，这些线程继续运行。
6. 保留状态锁、条件变量、操作超时、会话代次检查和资源关闭屏障。媒体流水线继续优先使用 GraphEngine。

“直接用锁”不等于所有操作使用同一把锁。操作锁负责控制 API 不并发；状态锁负责调用线程与后台线程交换状态。后台完成路径绝不能获取操作锁。

本次不扩展或改造 utoolkit 的通用 `postSyncTask()`。底层 WebRTC、设备及已有通知投递所需的执行设施按现有职责保留；不新增任务队列，不把房间事件循环迁移成 TaskQueue，也不将媒体数据搬进同步控制调用链。

## 2. 范围与当前实现

当前 `RtcEngine::Impl::submit()` 使用 CompletionSource 和 then 把工作投递到控制队列，并展开内部异步结果；这层整体移除，改为加锁后的普通函数调用。

当前 RoomEngineState 使用 `std::jthread`、已有命令邮箱、传输事件、唤醒和截止时间处理房间状态。保留这条事件循环和它的状态归属，不用新执行器替换它。内部向协议线程发送命令是必要的线程交接，不为每次公共调用建立新队列或新线程。

当前 RoomTransport::remove 的异步返回改为内部请求与解除绑定完成事件/信号，完成屏障不删除。现有媒体与通知线程继续按其职责运行。

主要修改位置：

| 内容 | 路径 |
| --- | --- |
| 结果和错误类型 | `include/livekit/async_result.hpp`、`error.hpp`，拟新增 `result.hpp` |
| 引擎公开接口 | `include/livekit/rtc/engine.hpp`、`graph_api.hpp`、`media_config.hpp` |
| 引擎实现 | `src/engines/rtc/rtc_engine_impl.hpp`、`rtc_engine.cpp`、`pipeline_engine.*` |
| 房间与轨道接口 | `include/livekit/rtc/room_engine.hpp`、`stream_pipeline.hpp` |
| 房间与传输 | `src/backends/rtc/room_engine.cpp`、`room_transport.hpp`、`livekit/livekit_transport.cpp` |
| 执行器 | `include/livekit/rtc/executor.hpp`、`src/engines/rtc/executor.cpp` |
| 调用方 | SDK 测试、示例、CLI，以及 livekit-app 的会议、投屏、远控和直播应用 |

只迁移本次 RTC 控制链及其依赖。普通状态查询、数据通路及无关的 Player、Studio、ServerSDK 接口不批量改签名，第三方内部 future 也不要求全部删除。

## 3. Result 契约

项目保持 C++20，增加小型 `Result<T>`，不为使用 std::expected 升级语言标准。

- `Result<T>::success(value)` / `ErrorCode::ok` 表示成功。
- `Result<T>::failure(record)` 接受非空错误记录。
- `has_value()`、显式布尔转换判断状态。
- `operator*` / `operator->` 访问成功值，支持 move-only 值。
- `error()` 返回 `const FailureInfo&`，在失败分支使用。
- 可提供 `value_if()` / `error_if()` 以空指针表示当前没有对应状态。

不提供 wait、get、then、as_future 或抛异常的 value。错误状态下解引用属于调用契约错误，用断言或前置条件约束。无数据操作直接返回 `ErrorCode`，不提供 Result 的成员方法，应显式比较 `ErrorCode::ok`。

错误复用现有 FailureInfo，可共享不可变错误记录，保留 id、domain、code、message、retryable、backend_reason 以及 operation/track_id/epoch 等上下文。同一故障在结果、事件和快照间传播时保留关联 ID。

预期业务失败直接构造 Result，包括参数非法、状态不允许、已关闭、资源上限、超时及已有控制操作的取消。线程误用和非法重入使用明确的错误码，必要时补充 `invalid_thread`、`invalid_state`，不全部归为 internal_error。

第三方及遗留异常在操作/线程边界转为错误，不向调用方抛出业务异常。不要机械地给所有 API 标记 noexcept；错误对象分配及不可恢复故障要单独审查，避免把报告错误变成进程终止。

### 3.1 接口分类

| 接口性质 | 返回形式 |
| --- | --- |
| 生命周期、配置、发布订阅等可失败操作 | Result<T> / ErrorCode |
| 普通属性、已有安全快照 | 直接返回值 |
| 未找到属于正常情况的查询 | std::optional<T> |
| 高频帧读写、背压与流结束 | 保留 WriteStatus、ReadResult 等专用类型 |
| 析构 | 无返回值，内部完成安全清理 |
| 房间状态变化、运行期故障、媒体输出 | 保留已有事件/媒体回调 |

初始化可能失败的公共工厂改为返回 `Result<std::unique_ptr<RtcEngine>>` 等，避免调用方捕获构造异常。现有 services 等可能报告能力错误的接口按实际失败语义迁移，不要求所有查询都套 Result。

### 3.2 目标接口与调用

```cpp
// RtcEngine
ErrorCode join(RoomOptions options);
ErrorCode leave();
ErrorCode close();
ErrorCode enable_media(AudioOutputOptions options = {});
Result<TrackId> publish(TrackDescriptor descriptor);
ErrorCode unpublish(const TrackId& id);
ErrorCode unsubscribe(const TrackId& id);
ErrorCode set_muted(const TrackId& id, bool muted, ControlOptions options = {});
ErrorCode set_receive_demand(
    const TrackId& id, ReceiveDemand demand, ControlOptions options = {});

// RoomEngine
ErrorCode join(RoomOptions options);
Result<std::shared_ptr<PublishedTrack>> publish(TrackDescriptor descriptor);
Result<std::shared_ptr<SubscribedTrack>> subscribe(TrackDescriptor descriptor);
ErrorCode leave();

// 保持普通查询和数据通路。
bool media_enabled() const;
RoomSnapshot snapshot() const;
WriteStatus write(const TrackId& id, EncodedFrame frame);
```

```cpp
auto joined = engine.join(options);
if (joined != ErrorCode::ok) {
  report_error(joined);
  return;
}

auto publication = engine.publish(descriptor);
if (!publication) {
  report_error(publication.error());
  return;
}
use_track(*publication);
```

轨道的 set_muted/set_demand/request_keyframe/close、RoomView 的控制接口、RtcGraphApi 及流水线关闭一并适配。媒体节点的实时反馈、关键帧和故障报告继续走内部非阻塞消息路径，不调用需要操作锁的公共控制接口。

## 4. 锁与执行模型

### 4.1 一把操作锁串行会话控制

同一会话只设置一把 `operation_mutex`。建议由 RoomEngine 的私有会话实现拥有，RtcEngine 通过内部访问复用；RoomEngine 独立使用时规则不变。不再引入 ControlDomain、专属控制线程或通用调度抽象。

操作锁保护完整控制调用，包括必要的网络结果等待和资源收尾。只有允许的外部调用线程及明确的内部同步控制路径可以获取它。

```cpp
// 示意，非完整实现。进入加锁前先检查回调/后台线程误用。
ErrorCode RoomEngine::join(RoomOptions options) {
  std::lock_guard lock(operation_mutex_);
  return join_locked(std::move(options));
}

ErrorCode RoomEngine::leave() {
  std::lock_guard lock(operation_mutex_);
  return leave_locked();
}
```

RtcEngine 已取得该锁时，调用 RoomEngine 的可信 `*_locked()` 内部方法，不调用会再次加锁的公共方法。流水线、RoomView 和轨道句柄要遵循同样规则，不使用 recursive_mutex 掩盖重复加锁。

调用者可来自不同应用线程，但执行期间只有一个控制操作。互斥锁不保证 FIFO 或严格公平，不把锁竞争顺序作为业务协议。

### 4.2 状态锁与条件变量

房间事件线程继续独占其协议状态机；调用线程通过现有内部命令交接发起请求。共享快照和本次操作结果使用短时状态锁保护。状态锁可复用现有共享状态保护，不为每个层次增加一套锁框架。

调用线程等待条件变量时释放状态锁，但继续持有操作锁：

```cpp
// 示意：调用方已持有 operation_mutex_。
std::unique_lock lock(state_mutex_);
const bool ready = completed_.wait_until(lock, deadline, [&] {
  return pending.result.has_value();
});
// 按 ready 和操作状态处理成功、失败或超时收尾。
```

后台完成处理只使用状态锁，先更新结果及可观察快照，再通知：

```cpp
{
  std::lock_guard lock(state_mutex_);
  // 检查 operation id / epoch，仅完成一次。
  pending.result = /* Result<T> */;
}
completed_.notify_all();
```

上例 pending 代表私有操作记录，含必要 ID、epoch、结果和完成状态。它由调用及后台路径安全持有，不公开、不包含 then，也不成为另一个 AsyncResult。超时返回后可能到达的消息不能引用失效栈对象。

### 4.3 不允许的等待关系

| 路径 | 规则 |
| --- | --- |
| 调用线程持操作锁等待网络 | 允许，后台推进不得需要操作锁 |
| 后台房间/网络线程获取操作锁 | 禁止，包括故障、超时、解除绑定完成路径 |
| 操作线程持状态锁执行网络 I/O、等待线程或调用第三方 | 禁止；释放状态锁后执行 |
| 图关闭等待媒体/输出线程 | 允许，但这些线程不得获取操作锁或等待该控制调用 |
| 公共用户回调重入控制 API | 在获取操作锁前返回明确错误 |
| 同会话内部嵌套控制 | 调用可信 *_locked 方法，不重复加锁 |
| 持一个会话的操作锁再获取另一会话的操作锁 | 禁止；共享采集/输出不通过这种方式编排 |

操作锁是专门允许跨操作等待持有的锁；这不意味着可以同时持有其他业务/状态锁等待。后台完成路径仅短暂持状态锁，条件变量等待释放状态锁，从而不形成相互等待。

用户 graph factory/hook 可能在持操作锁的调用线程执行：它们是用户回调上下文，必须禁止重入控制。通知、媒体回调同样不能同步调用控制接口，不能等待应用线程的 close 返回，也不能在回调内销毁引擎。检查在阻塞获取操作锁之前完成。

### 4.4 锁不提供线程亲和性

移除 require_owner 的队列检查后，改为内部调用约束和必要的持锁/回调上下文检查。不能据“串行执行”推断同一个 OS 线程。

审查 GraphEngine、设备、渲染、音频和 WebRTC 对象的创建/使用/销毁要求。确实要求固定线程的底层资源继续由原有执行线程管理，在其适配层交接，不让它们随着公共 API 调用线程任意迁移；也不为此恢复整套引擎控制队列。

## 5. RoomEngine 的同步操作

### 5.1 join 执行流程

```text
调用线程：检查调用上下文 → 获取操作锁 → 发出内部连接命令
                                      → 等待本次结果（释放状态锁）
                                      → 必要收尾 → 返回 Result → 释放操作锁

房间/网络线程：处理命令与连接事件 → 更新状态和结果 → 唤醒调用线程

其他调用线程：等待同一操作锁 → 执行 leave 或其他控制操作
```

私有 `join_locked()` 不要求新增公开或逐层回调式 start_join。保留现有房间后台循环，先登记本次连接操作再启动传输，以覆盖立即失败或快速成功。

后台处理连接成功时，更新快照和结果；启动失败、连接失败或超时时，进入相应状态及清理路径。调用方不需要定时扫描 future，也不需要专门等待线程。

### 5.2 其他操作及完成语义

| 操作 | 成功返回条件 |
| --- | --- |
| join | 当前会话达到既有连接成功条件，快照已经更新 |
| RoomEngine publish/subscribe | 端点收到原有成功确认并就绪 |
| RtcEngine 图/媒体发布订阅 | 端点就绪、图启动和登记完成 |
| 静音、接收需求、订阅权限 | 保留原契约的本地门控/信令提交含义，不冒充服务端 ACK |
| 成员信息更新 | 收到其要求的服务端 ACK |
| 轨道关闭、unpublish/unsubscribe | 停止入口，完成需要的图及原生解除绑定屏障 |
| leave | 本会话资源退出完成，允许后续重新 join |
| close | 终止引擎，完成资源及回调屏障，后续控制调用失败 |

成功不代表后续网络/媒体永不失败，运行期事件和快照继续报告变化。

### 5.3 后台循环与传输层

保留现有 jthread、唤醒及截止时间循环，不改成 TaskQueue、不引入周期扫描所有 pending 结果。已有命令/事件/媒体缓存保持容量限制，统计事件可合并，完成与终止消息不能因普通数据积压被静默丢弃。

joining、Entry、PendingControl 和 Removal 中的 CompletionSource 替换为简单的私有操作/完成记录。正常完成、错误与超时必须仲裁为唯一结果，epoch 和轨道 incarnation 继续过滤旧会话/同名旧轨道事件。

RoomTransport::remove 改成内部请求与完成事件/信号，提交成功不等于解绑结束。调用线程等待最终屏障时，房间及传输线程继续处理其他必要事件，不获取 operation_mutex。

传输 stop/join 的执行位置按现有依赖安排：等待的线程不能是被等待者，也不能让被等待者依赖操作锁。网络阶段和本地阻塞 I/O 必须有实际可工作的超时/终止机制，不能只给外层条件变量设置超时。

## 6. 入会、离会、超时与关闭

### 6.1 不抢占正在执行的操作

- join 持锁期间，leave 等待；join 成功、失败或超时收尾后，leave 才开始。
- leave 持锁期间，新的 join 等待；leave 完成后重新检查状态再执行。
- close 同样等待当前操作，不提前发信号打断 join/leave。取得锁后进入终止状态，后续等待者获得锁时返回已关闭错误。
- 不引入抢占取消通道、优先级任务或专门的取消线程。应用不能依靠 close 立即终止一个未完成的 join。
- 其他控制操作也统一串行：unpublish/unsubscribe 不再抢占正在等待 ACK 的 publish/subscribe，而是在前者完成或超时后执行。这是明确的并发语义变化，调用方与原有测试必须同步修改。

已有 ControlOptions::stop 如继续用于成员更新等特定控制请求，可由后台状态循环处理该请求自己的协作式取消；不扩展成入会/离会打断机制。普通 mutex 的获取等待不可取消，取得锁后先检查 stop 是否已经请求，避免发出已失效请求。

### 6.2 超时从操作实际开始计算

使用普通 mutex，不引入 timed_mutex 或新的排队调度器。获取操作锁的等待时间不计入本次操作的协议超时；取得锁并开始执行后建立单一绝对截止时间，各协议步骤共享它，不层层重置。

因此总调用耗时可能包括前一个操作的耗时、本次操作超时及安全清理时间；不承诺端到端总时长等于 RoomOptions::operation_timeout。应用如需严格排队预算，应在其工作调度层处理，本方案不增加该能力。

不需要主动打断不等于允许无限等待：连接、首条响应、ACK 等阶段要有有限截止时间。超时后在状态锁保护下撤销/终结对应操作，忽略或补偿迟到响应，完成必要收尾再返回。

超时本身不能证明后台已退出，不得释放仍被后台引用的对象。服务端可能已执行的请求保留 outcome_unknown，不自动重放。必要资源清理可能晚于协议截止时间，不宣称能够强杀任意第三方调用。

### 6.3 关闭顺序

1. 在允许的外部线程获取操作锁，等待当前操作自然结束。
2. 在短状态锁保护下标记离会或终止状态；close 为终止，leave 保留重入房能力。
3. 关闭媒体入口，停止生产者，等待编解码/源/输出线程退出或静止，停止并清理图。此时不持状态锁。
4. 完成轨道解除绑定，停止传输及会话资源；相关完成路径不获取操作锁。
5. 解除用户回调并完成必要 fence，保证成功返回后本次会话的回调不再访问调用方已释放的对象。
6. 更新终态并返回 Result。最终析构时再结束对象拥有的后台事件线程，释放资源。

具体媒体与端点停止顺序按依赖细化，保持 GraphEngine 原有生产者先静止的要求。操作锁可以覆盖清理，但所有被等待线程和回调都必须不获取这把锁。

重复 close 幂等；并发 close 通过操作锁先后执行并观察同一终态。leave 在已离会状态按既有幂等契约处理。失败返回要说明未完成的资源状态，不能释放仍在使用的内存。

析构前，应用必须停止并等待其他线程的成员调用。互斥锁不提供对象并发析构安全。析构不调用会重复加锁的公共关闭函数，不从回调/工作线程销毁自己，不让业务异常逃逸。

## 7. RtcEngine、媒体和故障处理

删除 RtcEngine::Impl 中的 control、submit、控制分发和专属线程 owner 检查；通过共用操作锁和内部 *_locked 方法顺序调用。保持房间状态循环和媒体模块职责边界。

发布流程直接写成：检查配置 → 发布端点并等待 → 创建/启动图 → 登记轨道 → 返回结果。失败执行图停止、输入租约释放和必要端点清理，再返回错误。

移除 MediaOperation.ready，以及仅用于提前返回的 pending 句柄。返回已完成的成功数据，例如：

```cpp
struct MediaPublication {
  TrackId id;
  std::shared_ptr<MediaTrack> track;
};

Result<MediaPublication> publish_media(MediaPublishSpec spec);
Result<MediaPublication> publish_video(MediaPublishSpec spec);
Result<MediaPublication> publish_audio(MediaPublishSpec spec);
// subscribe_media 使用对应的 MediaSubscription。
```

MediaTrack::throw_if_failed 改为返回 ErrorCode 的状态检查。运行期故障通过原有后台报告更新快照、停止继续接收/产生无效媒体并发出通知，不从故障线程同步进入加操作锁的图清理流程。

删除控制队列后，需要明确故障后的资源回收：后台先设置故障和停止标记、使等待中的操作失败；需要获取操作锁的完整图销毁由后续控制操作或显式 close/leave 执行。本方案不增加专属清理线程。应用可在错误通知后异步安排外部工作线程调用关闭，但通知回调本身不等待它。闲置故障对象保留的资源受原有轨道上限约束，不能继续无限创建；实现和测试须记录这项相对原自动清理的变化。

移除旧 RtcExecutor。应用直接使用同步 RtcEngine，UI 通过已有应用工作线程调用并异步回传结果；不要求每个调用临时创建线程，不向 SDK 增加另一套异步 API。

## 8. 明确不再实施的旧方案内容

- 不创建共享控制 TaskQueue/ControlDomain，不采用 invoke_sync、postSyncTask 或 *_on_control 作为控制 API 结构。
- 不把 RoomEngine 后台事件循环迁移成 TaskQueue。
- 不把通用 TaskQueue 的停止、异常和同步任务取消改造作为此次前置工作。
- 不实施 close/leave 在锁外抢占当前入会的机制。
- 不保留公共操作完成回调或另一个公开 future 外观。
- 不要求所有接口使用 Result，不改变普通快照和媒体数据的专用状态类型。

已有底层线程、通知投递和内部消息继续负责实际工作；“移除控制队列”不等于取消网络异步处理。

## 9. 实施与验证

### 9.1 迁移步骤

| 阶段 | 工作 |
| --- | --- |
| 1 | 增加 Result<T>/ErrorCode、错误转换和目标接口说明 |
| 2 | 确立会话操作锁、状态锁、内部 *_locked 调用关系和回调重入检查 |
| 3 | RoomEngine 复用现有后台循环，将 CompletionSource 改为私有结果记录及唤醒 |
| 4 | 传输解绑、轨道/RoomView 控制、超时及关闭屏障迁移 |
| 5 | RtcEngine/流水线/媒体同步编排，移除引擎控制队列及 RtcExecutor |
| 6 | 修改应用、示例和 CLI 的结果处理、工作线程调用与退出等待 |
| 7 | 清理 async_result.hpp、旧构建引用和测试，更新现行文档 |
| 8 | 运行相关编译、单元与无窗口集成验证，记录限制 |

保持现有代码规范，避免顺带重构通用任务队列、无关模块或媒体架构。修改代码前逐项检查固定线程归属和自动故障清理调用点，不能仅以机械替换返回类型完成迁移。

### 9.2 验证矩阵

| 类别 | 必须验证 |
| --- | --- |
| Result | void、move-only、成功/错误、错误关联及业务异常转换 |
| 操作串行 | 并发 join/leave 不重叠；leave 等待 join 成功、失败或超时；close 不抢占当前操作 |
| 锁正确性 | 后台完成不取操作锁，条件变量释放状态锁，内部 *_locked 不重复加锁 |
| 重入 | 通知/媒体/graph factory/hook 误调用在加锁前失败，内部合法嵌套可执行 |
| 超时 | 首条响应、连接、ACK 均可超时；锁等待不计入协议超时；迟到结果不访问失效记录 |
| 发布订阅 | 顺序发布、撤销等待前一个操作完成、同名轨道实例隔离、旧 epoch 补偿解绑 |
| 媒体 | GraphEngine 启停、建图失败回滚、源租约释放、线程亲和性和共享源跨会话无锁环 |
| 故障 | 后台报告不等待操作锁，失败及时可见，后续控制/关闭完成资源回收，故障对象不无限增长 |
| 关闭 | 输出/通知 fence、重复关闭、关闭与其他调用竞争、最终析构、没有遗留后台引用 |
| 应用 | UI 不调用阻塞控制接口，工作线程顺序处理结果，退出等待在途操作 |
| 网络集成 | 隔离 SFU 的多轨收发、退房重入、权限/成员 ACK、断线与迟到事件 |

使用假传输和可控 barrier/latch 验证“离会等待入会结束”，不要再把“离会必须立即取消入会”作为本方案测试预期。其他取消/撤销测试按上文新契约区分，不能直接删掉并发验证。

只运行受影响目标的编译和测试。本次不修改通用 TaskQueue 时，不把其全量改造验证列为前置。使用单元/集成测试、无窗口测试及必要的程序内渲染截图，遵守仓库 AGENTS.md，不使用 computer-use 或桌面自动化。

自动化结果与真实设备人工验收分开记录。静态锁关系审查不能替代并发测试；一次测试通过也不意味着证明任意第三方回调均无死锁。

## 10. Go SDK 参考与边界

核对来源为本机 `D:/workspace/server-sdk-go` 在 2026-10-29 检查到的源码，属于本地版本观察，不代表所有上游版本或完成了运行测试。

| 源码 | 观察到的行为 | 本方案借鉴或区别 |
| --- | --- | --- |
| `examples/echo/main.go` | 顺序调用 `JoinWithToken` 和 `PublishTrack`，以 `error` 判断失败 | 对应 C++ 的 `ErrorCode` 和 `Result<T>` |
| `room.go`：`JoinWithContextAndToken` | 支持 context，默认连接超时为 5 秒，可通过选项修改 | 借鉴同步返回及超时；本方案不要求入会/离会支持主动打断 |
| `localparticipant.go`：`PublishTrack` | 建立缓冲 channel，注册响应监听后发送请求，通过 select 等发布 ACK 或 10 秒超时 | 先登记再发请求；同步等待的完成由后台路径提供 |
| `engine.go`：`OnLocalTrackPublished` | 查找响应 channel，解锁后发送结果 | 完成信号不能依赖等待者继续执行其队列 |
| `signalling/signaltransport_websocket.go`：`Start` | 启动独立的 readWorker goroutine | 调用方阻塞期间，网络读取仍能推进 |
| `engine.go`：`Close`、`abortPendingRequests` | CAS 保证初次关闭动作；终止 pending request；另起 goroutine 执行部分 PC/信令关闭 | 关注幂等与唤醒；其返回不等价于本方案要求的完整同步关闭屏障 |

Go 中等待 channel 通常挂起当前 goroutine；本方案中 C++ 调用线程通过条件变量等待，后台房间/网络线程继续工作。两者都要求完成路径独立于等待者；本方案不使用引擎控制 TaskQueue。

本地实现也有需要区分的限制：

- `PublishTrack` 的这一处 select 只有发布响应和超时，没有直接监听调用方 context/关闭信号；不能从其他 pending request 的 abort 机制推断该等待也会立即被关闭打断。
- `RTCEngine.waitUntilConnected()` 向辅助函数传 `context.Background()`，辅助函数每 10 毫秒检查连接状态；连接剩余超时仍有限，但不能声称 Join 的每个阶段都直接响应调用方 context 取消。
- WebSocket 建连使用 `DialContext`，但成功后等待首条 Join 响应调用 `ReadMessage()`；该路径未设置读取截止时间或绑定 context 取消来关闭连接。对端升级连接后不返回响应时，存在单靠取消/连接超时无法结束这次读取的风险，需专门构造测试验证。
- 信令失败后的 validate 使用独立的 `context.Background()` 派生超时，可能在调用方取消后继续执行诊断请求；不能保证取消立即返回或始终保留 `context.Canceled`。
- `Close()` 的部分清理在 goroutine 中执行，`Disconnect()` 返回不证明所有底层关闭都结束。
- echo 使用不接收外部 context 的 `JoinWithToken`，且在入会与发布完成后才注册 SIGINT 通知，因此示例没有实现入会期间的协作式取消；可改用在入会前建立的 `signal.NotifyContext` 和 `JoinWithContextAndToken`，但上述 SDK 内部取消缺口仍需修复。
- echo 的 `ReadRTP()` 出错后继续循环，没有退出条件；终止性错误可能导致持续重试。示例还有共享的普通 bool，不能把它作为完整并发生命周期范例。
- 示例在错误分支使用 `panic(err)` 是示例应用的选择，不改变 SDK 返回 error 的接口形式。

借鉴其“同步接口、显式错误、后台推进、响应匹配”的结构；不照搬轮询、无退出读循环或异步关闭语义。此次没有修改或运行 Go 仓库。

## 11. 验收清单与当前状态

- [x] RTC 可失败控制 API 同步返回 Result，正常业务错误不抛异常。
- [x] RTC 控制链的 AsyncResult/CompletionSource 和旧执行器依赖清理完成；无关 Player 和 RoomServices 数据 future 保留。
- [x] RtcEngine 控制 TaskQueue 删除；没有新增控制任务队列或专属控制线程。
- [x] 一把会话操作锁串行控制，内部调用不重复加锁。
- [x] 后台完成、超时、故障报告和媒体工作不获取操作锁。
- [x] join/leave/close 等待当前操作，不依赖抢占取消。
- [x] 超时起点、迟到事件、撤销并发语义和故障延后回收已同步到调用方与测试。
- [x] 普通快照、媒体数据路径与 GraphEngine 已通过相关自动化检查；底层线程归属完成源码审查，真实设备另行验收。
- [x] 媒体/传输/通知关闭屏障完整，析构生命周期明确。
- [x] SDK、产品、示例、测试、构建和现行文档迁移完成并有验证记录。

截至本文更新：已完成 SDK、应用、示例与文档迁移。Windows Release 编译成功，SDK 契约测试 6/6、产品无窗口生命周期测试 2/2，以及六项隔离 SFU 集成通过。验证范围、日志、故障延后回收和线程归属审查见 [实施验证记录](rtc-sync-result-validation-20261029.md)。这些勾选表示本次实施及所列自动化检查完成，不代表真实设备、其他平台或任意第三方图节点均已验收。
