# RTC 同步控制与 Result 重构方案

日期：2026-10-29。

状态：设计方案，尚未实施。本次只记录目标接口、线程模型、迁移和验收要求，不代表代码已经具备这些行为，也不沿用历史测试结果作为本方案的验证证据。

## 1. 已确定的方向

1. 删除本项目 RTC 实现中的 `AsyncResult`、`CompletionSource` 以及基于 `.then()` 的操作编排，不保留这些类型的兼容层。
2. 公共控制 API 同步返回 `Result<T>`。成功时携带值，失败时携带结构化错误；`Result<void>` 同样携带失败信息。正常业务失败不通过异常传递。
3. 控制调用统一通过拥有专属线程的 `TaskQueue::postSyncTask()` 执行。同一控制域内的可信内部调用直接顺序执行。
4. `RtcEngine` 与其拥有的 `RoomEngine` 共用一条 SDK 控制队列。房间状态处理、底层传输和媒体执行独立推进，不依赖这条控制队列在等待期间继续执行任务。
5. 保留完成信号、网络事件、故障报告和取消通知。它们不属于同步控制调用链，不能反向同步调用正在等待的上层线程。
6. 媒体流水线继续优先使用 `GraphEngine`，保留源节点主动供帧、逐轨隔离、有界队列和原有关闭屏障。

“全部使用 sync”指控制 API 的跨线程调用方式，不表示逐帧传输、网络回调、故障通知也必须使用同步调用。结果类型与执行方式是两件事：`Result` 是已经完成的值，不负责调度或等待。

本方案取代前面讨论的逐层 `start_join(..., completion)` 作为主要业务组织形式。公开代码和引擎控制代码使用普通顺序调用；协议状态机仍保存进行中的请求及完成信号，不将其重新包装为通用异步框架。

## 2. 当前实现与需要改变的部分

当前 `RtcEngine::Impl::submit()` 通过一个立即完成的 `CompletionSource<void>` 触发 `then()`，把工作投递到引擎控制队列；内层异步结果再展开为外层结果。

当前 `RoomEngineState` 自带 `std::jthread`，通过 `commands`、传输事件、唤醒信号和截止时间推进状态机。连接、发布、控制请求和解除绑定分别保存待完成记录。用户通知使用独立的 TaskQueue。

当前 `RoomTransport::remove()` 返回 `AsyncResult<void>`，传输线程结束原生解除绑定后完成结果。该完成屏障必须保留。

现有 `TaskQueueSTD::postSyncTask()` 只能表示任务函数已经执行完，不能自动等待函数发起的网络操作。其同队列分支直接执行任务；跨线程分支等待 `SyncTaskWrapper` 设置完成标记。目前仍需加固异常、停止及同步任务被丢弃时的唤醒路径。

主要代码位置：

| 内容 | 路径 |
| --- | --- |
| 当前异步结果实现 | `include/livekit/async_result.hpp` |
| 错误模型 | `include/livekit/error.hpp` |
| 引擎控制实现 | `src/engines/rtc/rtc_engine_impl.hpp`、`rtc_engine.cpp` |
| 房间状态机 | `src/backends/rtc/room_engine.cpp` |
| 传输接口与 LiveKit 实现 | `src/backends/rtc/room_transport.hpp`、`livekit/livekit_transport.cpp` |
| 同步队列实现 | 聚合仓库 `graphflow-cpp/utoolkit/task_queue/` |

## 3. Result 与错误契约

项目当前使用 C++20。本次实现一个小型 `Result<T>`，不为使用 `std::expected` 升级语言标准。以后可在保持语义的前提下替换实现。

建议新增 `include/livekit/result.hpp`，提供：

- `Result<T>::success(value)`，`Result<void>::success()`。
- `Result<T>::failure(record)`，其中错误记录必须非空。
- `has_value()` 和显式布尔转换。
- `operator*` / `operator->` 访问成功值，提供适当的移动语义。
- `error()` 返回 `const FailureInfo&`，仅在失败状态调用。
- `value_if()` / `error_if()` 可提供不满足状态时返回空指针的安全探测。

类型支持 move-only 的成功值。成功与失败互斥，不提供容易误用的默认成功值；访问错误状态的值属于调用契约错误，用断言或前置条件约束，不提供会抛异常的 `.value()`。不包含 `wait/get/then/as_future`。

错误复用 `FailureInfo`，内部可以持有 `std::shared_ptr<const FailureInfo>`，保留 `id`、`domain`、`code`、`message`、`retryable`、`backend_reason` 和 `FailureContext`。结果、事件和轨道状态中的同一故障保留关联 ID，补充上下文时不生成另一个故障 ID。

参数非法、状态不允许、队列满、关闭、超时、取消直接生成失败结果。建议为线程误用/非法重入及非法状态补充明确错误码，名称在实现时统一，例如 `invalid_thread`、`invalid_state`，不要一概归为 `internal_error`。

第三方和遗留代码的异常在操作及线程入口边界转换为错误。SDK 控制流程不通过 `throw SDKError` 表达预期失败。不要仅给所有 API 添加 `noexcept`：错误记录分配、第三方调用与不可恢复故障需要单独审查；不能以进程终止代替返回可报告的失败。

## 4. 哪些接口使用 Result

| 接口性质 | 形式 | 说明 |
| --- | --- | --- |
| 生命周期、配置、发布订阅等可能失败的控制操作 | `Result<T>` / `Result<void>` | 本次迁移的主体 |
| 普通属性、已有安全快照 | 原有值类型 | 不制造不存在的失败状态 |
| 查询不存在是正常情况 | `std::optional<T>` | 不能用它丢弃操作错误 |
| 高频帧读写、背压和流终止 | `WriteStatus`、`ReadResult` 等 | 保留领域语义，不转到长时间等待的控制队列 |
| 析构 | 无返回值 | 完成安全清理，诊断不能逃逸为异常 |
| 会话状态变化、运行期故障、媒体输出 | 保留事件或媒体回调 | 不用它们代替单次操作返回值 |

公共工厂负责可失败的初始化，目标形式为 `Result<std::unique_ptr<RtcEngine>>` 和对应的 RoomEngine 工厂。构造过程收进工厂，避免要求调用方捕获初始化异常。`services()` 等目前可能抛出业务错误的接口需要逐一审查；不支持某项能力时应通过 `Result` 返回明确错误。

迁移范围包括所有依赖 `AsyncResult` 的 SDK 源码和调用方。Player、Studio、ServerSDK 及 RoomServices 其他既有协议不因共享仓库而一律改签名；需要为本次依赖、回调和关闭适配时再做明确的局部修改。第三方 future/回调无需全部重写，不能把 `AsyncResult` 简单换成公开的 `std::future`。

### 4.1 目标接口示例

```cpp
// RtcEngine
Result<void> enable_media(AudioOutputOptions options = {});
Result<void> join(RoomOptions options);
Result<void> leave();
Result<void> close();
Result<TrackId> publish(TrackDescriptor descriptor);
Result<void> unpublish(const TrackId& id);
Result<void> unsubscribe(const TrackId& id);
Result<void> set_muted(const TrackId& id, bool muted, ControlOptions options = {});
Result<void> set_receive_demand(
    const TrackId& id, ReceiveDemand demand, ControlOptions options = {});

// RoomEngine
Result<void> join(RoomOptions options);
Result<std::shared_ptr<PublishedTrack>> publish(TrackDescriptor descriptor);
Result<std::shared_ptr<SubscribedTrack>> subscribe(TrackDescriptor descriptor);
Result<void> leave();

// 普通读取和媒体数据路径不统一包装为 Result。
bool media_enabled() const;
RoomSnapshot snapshot() const;
WriteStatus write(const TrackId& id, EncodedFrame frame);
```

轨道句柄的 `set_muted/set_demand/request_keyframe/close`、RoomView 的可失败控制接口、RtcGraphApi、流水线的关闭接口同步迁移为 `Result`。框架内部媒体节点使用专门的非阻塞消息通道提出关键帧、反馈和故障请求，不从媒体工作线程调用公共同步控制接口。

```cpp
auto joined = engine.join(options);
if (!joined) {
  report_error(joined.error());
  return;
}

auto published = engine.publish(descriptor);
if (!published) {
  report_error(published.error());
  return;
}
use_track(*published);
```

### 4.2 每个成功结果的含义

| 操作 | 成功返回的边界 |
| --- | --- |
| `join` | 当前会话达到现有契约定义的连接成功状态，快照已更新 |
| RoomEngine 发布/订阅 | 对应端点获得原有成功确认并就绪 |
| RtcEngine 图或媒体发布/订阅 | 端点就绪、图成功启动、轨道登记完成 |
| 静音、接收需求、订阅权限 | 保留原契约的本地变更及信令提交边界，不冒充服务端 ACK |
| 成员信息更新 | 保留其服务端 ACK 完成边界 |
| 轨道关闭、取消发布/订阅 | 本地数据入口停止，必要图清理及原生解除绑定屏障完成 |
| `leave` | 本次会话和媒体资源完成退房清理，后续允许重新 join |
| `close` | 引擎进入终止状态，完成其拥有资源的关闭屏障，后续控制请求失败 |

`Result` 成功不保证媒体后续永不失败。运行期错误继续进入快照和事件。

## 5. 专属线程与状态所有权

### 5.1 控制域

引入私有的控制上下文（下文称 ControlDomain），持有一条专属线程的 TaskQueue、提交门禁、活动操作标识、关闭状态及诊断计数。该名称不要求成为公共 API。

RoomEngine 创建时确定控制域；RtcEngine 接管 RoomEngine 时复用它，不再新建另一条控制线程。接管仅允许未激活、没有外部并发使用的实例，不迁移运行中对象的线程归属。工厂、旧 `RtcEngine(unique_ptr<RoomEngine>)` 路径和测试注入必须统一执行该约束。

顶层对象/控制上下文负责队列生命周期，借用模块无权销毁队列。轨道句柄通过弱引用或关闭状态保护访问，不能仅因保留句柄而使已关闭引擎重新工作。最终队列释放必须发生在外部拥有者线程。

| 执行位置 | 独占状态/职责 | 等待约束 |
| --- | --- | --- |
| SDK 控制 TaskQueue | 生命周期编排、媒体模块、图和轨道注册、同步业务步骤 | 可等待下层独立推进的操作；不得等需要自身继续取任务的结果 |
| 房间状态 TaskQueue | 连接状态、epoch、端点、ACK、超时、解除绑定记录 | 只运行短任务；不等待网络结果、图退出或上层控制线程 |
| 传输与 WebRTC 执行线程 | 信令、PeerConnection、网络、原生端点操作 | 不反向同步调用房间或 SDK 控制线程 |
| 通知 TaskQueue | 房间/业务事件和故障通知 | 用户回调及时返回；不调用阻塞控制 API |
| 采集、源节点、编解码、输出工作线程 | 媒体数据及处理 | 不同步反调控制线程，不等待引擎关闭 |
| 应用工作线程 | 调用同步 API、处理 Result | 可等待 SDK 操作；不能持有 SDK 回调所需的锁 |

复用现有媒体和传输执行资源，不按每次操作增加线程。底层 Pion/WebRTC/设备线程模型不强行替换为本项目 TaskQueue。专属线程数量、队列深度和等待时长应可诊断。

### 5.2 同步调用方向

```text
应用工作线程
    │ postSyncTask（整个控制调用）
    ▼
SDK 控制线程
    │ postSyncTask（只提交/查询短状态任务）
    ▼
房间状态线程
    │ 必要的短下层控制请求
    ▼
传输/原生执行线程

结果与事件向上：有界消息、完成信号、唤醒；禁止反向同步等待。
```

图中的每条同步边都必须在实现审查中确认，不能因为下层排位较低就允许长时间阻塞房间状态线程。原生线程之间既有调用关系也必须计入等待图。

同一控制域内的可信 `*_on_control()` 直接调用，不二次排队。公共同步入口需先检查调用上下文，再判断 `isCurrent()`；用户 graph factory/hook 即使正在控制线程执行，也不能借此绕过重入禁止。以回调上下文标记、线程角色和活动操作状态检查非法调用，返回明确错误。

禁止 SDK 控制线程之间跨房间同步互调。共享采集/输出资源不能在持锁时逐个同步调用房间控制域；它们通过独立服务状态与非阻塞报告维持共享生命周期。

### 5.3 必须保持的无环条件

1. 不持有业务互斥锁进行跨线程 sync、等待操作、join 线程或执行用户回调。
2. 房间状态线程不等待 SDK 控制线程；传输线程不等待房间状态线程消费其结果。
3. 控制线程等待的完成信号由下层直接产生，不能先投递回控制队列或等待用户回调后再产生。
4. 图停止会等待的媒体线程不能同步请求该图所在控制队列。故障报告提交后立即返回。
5. 通知回调不能调用同步控制 API，也不能通过应用线程间接等待同一个 close 完成。回调更新 UI 使用异步投递，不阻塞等待 UI。
6. 队列不得在自身线程销毁。对象析构前，调用方停止外部并发访问；关闭支持并发调用不意味着允许任意线程并发析构对象。
7. 不用在等待期间嵌套执行任意控制队列任务来“解锁”，避免不可控重入。

专属线程不是无死锁证明；上述约束、运行期检查、第三方依赖审查及并发测试共同构成验收依据。

## 6. TaskQueue 同步基础设施加固

这是 SDK 迁移前置工作。通用队列不依赖 SDK 的 `Result` 或 `FailureInfo`。

### 6.1 任务与队列状态

同步任务至少区分 queued、running、executed、failed、cancelled，终态恰好一次。队列至少区分 accepting、stopping、stopped。提交检查和入队必须与停止过程原子协调。

- 未受理：队列已停止或容量不足，立即报告拒绝。
- 已受理未开始：关闭时明确执行或取消，不能静默销毁后让调用方永久等待。
- 已开始：完成或失败后才结束同步调用；取消请求只触发协作式结束，不能假装任务已经退出。
- 异常：在线程入口兜底，完成唤醒；通过通用执行状态和捕获异常信息交给 SDK 适配为 `Result`。
- 同队列执行：也受错误处理和 SDK 的重入约束保护。

为 `postSyncTask()` 明确增加可观察的执行/拒绝结果，或提供等价的通用同步调用出口；修改 TaskQueueBase、STD 实现、包装层和受影响调用点。SDK 的 `invoke_sync()` 只是执行和错误映射的薄封装，不实现第二套队列。

### 6.2 等待状态的生命周期

同步包装不能依赖会提前消失的调用者栈指针。共享等待状态持有任务终态、结果/异常及条件变量。队列取消后要先保证未开始的任务不会再运行，才能唤醒其调用者。

不要给 `postSyncTask()` 简单增加“等待超时立即返回”：如果任务仍在运行并引用调用者栈，会产生悬空访问。业务截止时间用于请求协作式取消；运行中的任务完成安全收敛后，同步函数才返回。任务参数、结果和必要资源必须拥有明确生命周期。

队列停止不是对象并发销毁许可证。关闭门禁、活跃调用租约及最终析构顺序保证调用者不会在队列已释放后进入成员方法。

### 6.3 容量与系统任务

保留现有用户命令、待操作、待解绑和通知容量约束，初始采用现有合理上限（例如现有 1024 级操作上限），避免因改同步而丢失资源限制。调用方线程数不等于队列容量，超额调用仍需被拒绝。

取消、终止、故障和完成通知不能与普通用户任务争抢最后一个容量槽。用按活动操作有界的专用信号/邮箱或保留系统容量，防止队列满后无法终止。

## 7. RoomEngine 改造

### 7.1 控制接口

公共 `join()` 经 SDK 控制队列的 sync 入口执行 `join_on_control()`。后者同步向房间状态队列提交启动步骤，收到受理结果和私有操作记录后，在 SDK 控制线程等待该记录完成。

私有操作记录包含操作 ID、epoch、必要的轨道实例标识、截止时间、取消状态、结果和完成信号。它是协议状态机记录，不公开，不提供链式接续或任意回调注册。

```text
应用调用 join()
  → SDK 控制队列执行 join_on_control()
  → 房间状态队列执行短启动任务
      检查状态、设置 connecting、登记操作、启动 transport
  → 启动任务返回，房间状态队列继续运行
  → SDK 控制线程等待操作记录
  → 网络事件到达，房间状态队列更新状态及快照
  → 写入 Result 并唤醒
  → 控制任务完成，postSyncTask 返回应用
```

先登记请求及完成状态，再允许传输响应进入处理流程，防止快速完成或同步失败遗漏等待者。启动失败必须清除登记并完成错误结果。

### 7.2 状态循环迁移

将当前永久 `run()` 循环分解为有界的状态队列任务。传输、待发帧、反馈、图故障和解除绑定结果使房间队列变为可运行；无事件时不轮询。

- 唤醒事件到 TaskQueue 采用合并调度，避免每帧无限投递任务。
- 邮箱写入、pending 标记清理和再次检查必须保证不漏唤醒。
- 每轮处理限制数量或工作预算，剩余工作重新调度，让取消和超时有机会执行。
- 最近截止时间使用延迟任务；延迟任务带代次标识，重入房或改期后忽略过期任务。
- 保留现有 dirty-work/deadline 管理能力，不退回固定间隔扫描所有 future。
- 统计事件可合并；终止与完成事件使用独立准入保障，不被媒体事件风暴淹没。

### 7.3 现有记录的替换

| 当前记录 | 目标 |
| --- | --- |
| `joining` | 当前连接操作记录 |
| `Entry::completion` | 发布/订阅操作 ID 及状态记录 |
| `PendingControl::promise` | ACK 请求记录和类型化结果 |
| `Removal::waiters` | 等待同一原生解除绑定屏障的操作记录 |
| `Command::cancel(exception_ptr)` | 已受理命令的取消/拒绝状态，映射结构化错误 |

房间状态线程负责正常完成、超时和取消的仲裁；入口关闭/拒绝与操作登记之间通过统一门禁协调。完成恰好一次，通知前更新可观察快照。旧 epoch、已取消请求和同名旧轨道的迟到 ACK 不得完成新操作。

### 7.4 传输层

`RoomTransport` 的直接控制请求改为可报告受理失败的同步调用；需要网络 ACK 或原生解除绑定的操作通过请求 ID + 传输事件交付最终结果。例如 remove 的提交成功不等于解除绑定完成，必须有匹配的解除绑定完成事件。

房间状态线程不在 remove 中等待传输线程结束清理。传输线程通过有界的完成邮箱向上报告，房间状态线程更新待解绑记录，再唤醒 SDK 控制线程。

传输 stop 的阻塞 join 放在 SDK 控制线程的清理编排中，或拆成状态线程的短停止请求与控制线程等待终止屏障；不能把无限等待放进房间事件任务。保留最终原生资源关闭屏障。

## 8. RtcEngine、媒体和执行器

删除 `Impl::submit()` 的 admitted/then 技巧，以 `invoke_sync()` + 可信 `*_on_control()` 组织业务步骤。保持状态和资源所有权明确，不仅仅替换返回类型。

发布媒体按顺序执行端点发布、建立 GraphEngine、启动、登记成功。失败在控制线程完成图停止、输入租约释放和必要端点清理，再返回对应错误。运行期故障使用非阻塞消息，先影响媒体状态和下层待操作，再安排控制线程清理，不能依赖被占用的控制队列才解除等待。

移除 `MediaOperation.ready` 和只为提前返回而创建的 pending 句柄。建议提供普通成功数据：

```cpp
struct MediaPublication {
  TrackId id;
  std::shared_ptr<MediaTrack> track;
};

Result<MediaPublication> publish_media(MediaPublishSpec spec);
Result<MediaPublication> publish_video(MediaPublishSpec spec);
Result<MediaPublication> publish_audio(MediaPublishSpec spec);
// 订阅使用对应的 MediaSubscription 数据结构。
```

成功句柄已经初始化；`MediaTrack::throw_if_failed()` 改为返回 `Result<void>` 的状态检查接口。快照仍保留运行期错误。

`RtcExecutor` 当前将用户函数投递到引擎控制队列并展开异步结果。新接口下不保留这一套公开执行器，迁移调用方直接调用同步 RtcEngine；应用自己的有界工作队列负责后台操作。将必要的容量、统计与关闭能力迁入控制域，删除旧执行器及无用构建引用。

## 9. 取消、超时、退房与终止

### 9.1 不能把取消排在被取消操作后面

控制线程会等待整个操作，因此以下入口必须先通过独立且幂等的取消通道发出信号，再进入同步控制队列做正式清理：

- `close()`：关闭准入，取消当前会话所有进行中及待执行操作。
- `leave()`：建立本次退房门禁，取消本会话操作，完成后允许新的 join。
- `unpublish()/unsubscribe()` 和轨道 `close()`：按请求、epoch、实例身份取消对应的尚未完成操作。
- `ControlOptions::stop`：停止回调只设置取消状态并唤醒，不执行 sync 或持有房间业务锁。

操作提交、身份分配和关闭/取消标记需要一个短时准入临界区，保证取消不会漏掉“刚受理尚未进入控制队列”的请求。针对 TrackId 的操作必须明确目标实例；不能误取消同名后继轨道。关闭操作还需阻止新的 join/publish 插入被取消操作与清理任务之间。

取消标记不是完成结果。活动任务停止使用相关资源、完成必要的清理或将资源转交仍存活的回收状态后，才允许返回。迟到 ACK 仍需补偿解绑，不能再次启动图。

### 9.2 截止时间

为可等待的操作保存单一绝对截止时间，覆盖准入后排队、信令和 ACK 等协议步骤，避免每一层重置完整超时。已有参数通过 `RoomOptions::operation_timeout` 与 `ControlOptions` 传递；缺少配置的操作在实现阶段补足一致的默认策略。

排队中的超时请求在开始执行前被拒绝；进行中的请求被协作式取消。用于确保资源安全的收尾可能晚于协议截止时间，不能宣称超时能强行终止任意第三方线程。不能靠轮询超时规避生命周期问题。

服务端已收到请求时，超时/取消可能代表远端结果未知，保留 `outcome_unknown`，不自动重放非幂等操作。

### 9.3 关闭顺序

1. 从允许的外部调用上下文关闭准入并发送取消信号。
2. 当前活动操作结束；已排队普通操作返回取消/已关闭结果。重复关闭共享同一终止状态，不重复清理资源。
3. 控制线程停止媒体生产者，解除用户回调注册，等待被允许的在途回调与媒体工作完成，停止并拆除图。
4. 在仍存活的房间状态队列上处理端点清理、完成必要请求；停止并等待传输资源收敛。
5. 完成通知 fence，确保关闭成功返回后不再调用本次会话的用户回调。
6. 终止房间事件调度和延迟任务；保留可查询的终态或必要关闭记录。
7. 外部拥有者停止并销毁控制队列，最后释放对象。

具体媒体与端点屏障按依赖细化，但不能先销毁共享控制队列再调用 room 的同步 leave。显式 close 报告错误并完成可安全完成的清理；失败返回不宣称所有资源屏障已达成，更不能释放仍被线程访问的内存。析构沿同一安全清理路径，无抛出业务异常。

通知/媒体回调中不允许析构引擎或同步 close。保留回调替换/清空的 fence 语义，自回调注册修改需明确定义为拒绝或延后，不同步等待自身退出。

## 10. Go SDK 参考与边界

核对来源为本机 `D:/workspace/server-sdk-go` 在 2026-10-29 检查到的源码，属于本地版本观察，不代表所有上游版本或完成了运行测试。

| 源码 | 观察到的行为 | 本方案借鉴或区别 |
| --- | --- | --- |
| `examples/echo/main.go` | 顺序调用 `JoinWithToken` 和 `PublishTrack`，以 `error` 判断失败 | 对应 C++ 的 `Result<void>` 和 `Result<T>` |
| `room.go`：`JoinWithContextAndToken` | 支持 context，默认连接超时为 5 秒，可通过选项修改 | 同步 API 仍需截止时间和取消，不要求暴露异步结果 |
| `localparticipant.go`：`PublishTrack` | 建立缓冲 channel，注册响应监听后发送请求，通过 select 等发布 ACK 或 10 秒超时 | 先登记再发请求；同步等待的完成由后台路径提供 |
| `engine.go`：`OnLocalTrackPublished` | 查找响应 channel，解锁后发送结果 | 完成信号不能依赖等待者继续执行其队列 |
| `signalling/signaltransport_websocket.go`：`Start` | 启动独立的 readWorker goroutine | 调用方阻塞期间，网络读取仍能推进 |
| `engine.go`：`Close`、`abortPendingRequests` | CAS 保证初次关闭动作；终止 pending request；另起 goroutine 执行部分 PC/信令关闭 | 关注幂等与唤醒；其返回不等价于本方案要求的完整同步关闭屏障 |

Go 中等待 channel 通常挂起当前 goroutine，不需要永久占用一条专属 OS 线程；C++ TaskQueue 的阻塞会占住该队列唯一执行线程。因此不能把 Go 调用方式机械翻译为“所有网络事件也在同一同步队列执行”。

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

## 11. 迁移范围与顺序

| 阶段 | 工作 | 完成标准 |
| --- | --- | --- |
| 1 | Result、错误码和 TaskQueue 同步状态机 | 类型及队列契约测试通过，再迁移上层 |
| 2 | 控制域、准入/取消通道、线程角色与重入检查 | 共享控制队列和等待关系明确，独立 RoomEngine 可创建/关闭 |
| 3 | 房间事件队列、传输请求/完成事件 | 原状态机语义、截止时间、晚到事件和解绑屏障保留 |
| 4 | RoomEngine/轨道/RoomView 同步接口 | 连接、发布、取消、关闭返回 Result，无 AsyncResult 依赖 |
| 5 | RtcEngine、图与媒体接口 | 顺序编排、失败回滚、MediaOperation 替换、Executor 迁移 |
| 6 | CLI、示例、会议/投屏/远控/直播应用 | 去掉 get/wait_for/then 与 pending future 容器，界面无阻塞控制调用 |
| 7 | 清理构建与文档 | 删除 async_result.hpp 及其测试目标，更新现行 API 文档和例子 |
| 8 | 集成验收 | 下述测试矩阵及受影响目标构建完成，记录证据与未验收项 |

应用已有工作线程优先复用，不为每次 join/publish 临时启动线程。UI 完成回传使用应用自己的消息机制，SDK 不提供第二套异步控制外观。迁移并发测试时不能将原有“先发起再触发取消”用例直接改成单线程同步等待，应使用受控测试线程及屏障模拟并发。

文档中的历史 AsyncResult 记录保留其历史性质。实施后更新 `architecture.md`、`rtc-media-engine.md`、`rtc-encoded-api.md`、README、示例和构建配置；不在尚未实现时把这些现行说明改写成已经完成。

## 12. 验证矩阵

| 类别 | 必须覆盖的行为 |
| --- | --- |
| Result | void、move-only 值、错误字段/关联 ID、非抛出访问语义 |
| TaskQueue | 同队列执行、不同队列 sync、任务异常、拒绝提交、停止与提交竞争、未执行任务取消唤醒、运行中任务不提前释放 |
| 房间 | join 成败/超时、发布和 ACK、队列满、重复请求、epoch/incarnation、旧消息补偿解绑 |
| 取消 | close/leave 打断 join，unpublish/unsubscribe 打断对应 ACK 等待，入队前后取消竞争，关闭与新 join 竞争 |
| 事件驱动 | 不漏唤醒、截止任务过期过滤、事件风暴下取消与超时不饿死、空闲无轮询、其他已运行轨道继续传输 |
| 线程约束 | 通知/媒体/graph factory/hook 中误调用立即失败，可信同队列调用成功，禁止跨房间同步环 |
| 清理 | 建图失败、源租约释放、输出 quiesce、通知 fence、同名轨道独立关闭、晚到解绑、重复 close、外部最终析构 |
| 应用 | 不阻塞 UI、应用事件/工作队列交接、错误展示、退出期间取消、多个调用线程与容量限制 |
| 网络集成 | 隔离 SFU 下连接、多轨、重订阅、退房重入、权限/成员 ACK、断线恢复和关闭 |

用可控假传输、latch/barrier 和进程级测试超时构造等待竞争，尽量不用固定 sleep 判断是否死锁。测试超时只用于发现失败，不作为实现正确性的证明。检查当前进程及遗留线程/操作计数，不能只验证函数返回。

执行受影响的 utoolkit、SDK 和产品构建，以及相关单元、无窗口集成和必要程序内渲染检查。遵循仓库 AGENTS.md，不使用 computer-use 或桌面自动化。自动化通过与物理摄像头、麦克风、扬声器人工验收分开记录；Windows 结果不能代替其他平台验证。

## 13. 验收清单与状态

- [ ] 所有本次 RTC 控制接口按约定返回 Result，普通查询与数据路径未被机械包装。
- [ ] AsyncResult/CompletionSource 运行代码及构建依赖清零；不保留旧兼容层。
- [ ] 共享控制队列在对象创建/接管阶段确定，队列不会先于借用模块销毁。
- [ ] 同步等待依赖图无反向边，回调重入与媒体关闭依赖均有明确检查。
- [ ] TaskQueue 每个已受理同步任务都达到唯一终态，无停止/异常导致的永久等待。
- [ ] 取消路径不排在被取消任务后面，结束等待不早于所需生命周期安全边界。
- [ ] Result 错误关联、远端未知结果、原有完成时机和清理语义保持正确。
- [ ] 事件驱动、资源上限和 GraphEngine 媒体架构保留。
- [ ] 应用、示例、测试、构建及现行文档完成迁移。
- [ ] 验证矩阵有实际记录；设备人工验收与未验证平台明确标注。

截至本文编写：仅完成方案及本地源码对照；实现、编译、运行测试和设备人工验收均未执行。文中的接口及私有名称是目标设计，不是当前可编译 API。
