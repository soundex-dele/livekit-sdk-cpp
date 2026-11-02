# LiveKit 客户端后端设计方案

日期：2026-11-02。状态：方案，尚未实现。

## 1. 目标与已确认约束

在 `src/backends/livekit/` 新建一套 LiveKit 客户端实现，参考 Rust SDK 的
`Room → Participant → Track / Publication` 对象模型，参考现有 C++ RoomEngine
的编码帧端点、队列、反馈和重连处理。

已确认的设计约束：

- 公共操作接口采用同步调用，不提供完成回调、Future 或协程接口。
- 保留 `RoomObserver` 通知成员、轨道、连接和传输反馈等持续事件。
- 直接输入和输出编码后的音视频帧，不依赖 GraphEngine。
- 不负责设备采集、音视频编码、解码或播放，应用自行提供这些能力。
- 无返回值操作使用 `void`，失败记录日志，不使用 `Result<void>`。
- `connect()` 返回 `bool`，让调用方明确判断连接是否成功。
- 返回业务对象且可能失败的操作使用 `Result<T>`；查询返回数据；帧读写返回专用状态。
- 使用 C++17，不使用异常传播业务错误。
- 日志统一使用 SDK 内的 `src/support/logger.h` 宏，该文件从
  `graphflow-cpp/utils/logger.h` 原样复制。

本方案按用户明确要求，对本模块采用上述错误返回方式和无 GraphEngine 的媒体边界。
这覆盖仓库代码规范中同步可失败接口返回显式错误、流水线优先使用 GraphEngine 的默认约定。
其他命名、格式、所有权和验证规范继续适用，既有模块不因此改变接口。

## 2. 与 Rust 和现有 RoomEngine 的关系

| 设计维度 | 本方案 |
| --- | --- |
| 对象组织 | 参考 Rust，房间包含成员，成员管理发布，媒体轨道与发布对象分离 |
| 公共执行方式 | 同步返回；Observer 承载持续事件 |
| 媒体边界 | 参考 RoomEngine，直接收发编码帧 |
| 编码职责 | 应用负责，本模块不接收 PCM 或原始视频作为媒体入口 |
| 传输职责 | 编码帧适配、RTP/RTCP、拥塞反馈、网络加密和 WebRTC 连接 |
| 房间状态 | 新 RoomSession 独立管理，不包装现有 RoomEngine 来镜像状态 |
| 底层复用 | 协议、网络、WebRTC 及可抽取的编码帧传输组件 |

Rust `basic_room` 示例向 NativeAudioSource 输入 PCM，再由底层 WebRTC 编码。
本方案只借鉴其对象职责，不复刻该原始帧到编码器的媒体路径，也不移植示例中的
固定等待、发布顺序和诊断开关。

已有 `src/backends/rtc/`、RoomEngine 和 RtcEngine 保持原有用途。
新实现初期与其并存；是否迁移应用由后续任务决定。

## 3. 目录和构建边界

拟议布局如下，目录名表示规划，不表示这些文件已经存在：

```text
livekit-sdk-cpp/
├─ include/livekit/client/
│  ├─ room.hpp
│  ├─ room_options.hpp
│  ├─ room_observer.hpp
│  ├─ room_event.hpp
│  ├─ participant.hpp
│  ├─ local_participant.hpp
│  ├─ remote_participant.hpp
│  ├─ track.hpp
│  ├─ track_publication.hpp
│  ├─ encoded_frame.hpp
│  └─ types.hpp
├─ src/backends/livekit/
│  ├─ room.cpp
│  ├─ room_session.hpp/.cpp
│  ├─ participant/
│  ├─ track/
│  ├─ publication/
│  ├─ signaling/
│  ├─ transport/
│  └─ services/
├─ examples/basic_room_encoded/
└─ tests/livekit_client/
```

- 公共命名空间建议为 `livekit::sdk::client`，与既有 `rtc` 接口区分。
- 新增 `LiveKit::Client` 构建目标和 `LIVEKIT_BUILD_CLIENT` 开关。
- 公共头文件不暴露 WebRTC、Protobuf 或后端实现类型。
- 最终构建依赖不包含 RoomEngine、RtcEngine、GraphEngine 和采集/播放组件。
- 编码帧传输复用目标应从旧 RTC 实现中拆出中立依赖，不通过链接整个 RTCBackend 规避拆分。
- 没有 `ClientMedia` 或媒体图编排目标；测试程序使用的解码器属于测试依赖。
- 日志头文件已复制到 `src/support/logger.h`；后续 `LiveKit::Client` 私有链接
  独立的 `utoolkit_logging` 目标，不为日志引入 GraphEngine。

## 4. 对象职责

| 对象 | 职责 |
| --- | --- |
| Room | 连接、关闭、房间信息、成员集合、Observer 注册 |
| RoomSession | 内部唯一会话所有者，管理状态机、请求、传输和重连 |
| LocalParticipant | 发布/取消发布轨道、发送数据、更新本地成员信息 |
| RemoteParticipant | 远端成员信息和发布列表 |
| LocalAudioTrack / LocalVideoTrack | 声明编码格式，接收编码帧，维护本地轨道状态和发送反馈 |
| LocalTrackPublication | 本地轨道在房间中的发布状态、服务端 SID、发布参数 |
| RemoteTrackPublication | 描述远端发布，控制订阅和接收需求 |
| RemoteAudioTrack / RemoteVideoTrack | 读取接收的编码帧、查询状态、请求视频关键帧 |

LocalTrack 和 RemoteTrack 作为音视频轨道的统一句柄，避免调用者在发布和查询时处理
两套重复接口。句柄复制引用同一个内部状态，不复制传输资源。

本地 Track 在发布前即可创建。Publication 在成功发布后建立，取消发布不会销毁
应用持有的 Track，允许后续重新发布。首版一个本地 Track 同时最多绑定一个房间中的
一个有效发布，重复或跨房间并发发布明确拒绝。

区分本地稳定 TrackId、服务端 TrackSid、成员 identity 和成员 SID。
服务端 SID 可以在重连后改变，不能作为本地 Track 的永久身份。

## 5. 公共接口草案

以下声明用于确定职责和返回语义，不是可独立编译的完整头文件。
辅助类型的字段、include 和访问控制在实施阶段补齐。

```cpp
class RoomObserver {
 public:
  virtual ~RoomObserver() = default;
  virtual void on_room_event(const RoomEvent& event) {}
};

class Room {
 public:
  void set_observer(RoomObserver* observer);
  bool connect(ConnectionOptions options);
  void close();

  ConnectionState connection_state() const;
  RoomInfo info() const;
  LocalParticipant local_participant() const;
  std::vector<RemoteParticipant> remote_participants() const;
};

class LocalParticipant {
 public:
  Result<LocalTrackPublication> publish_track(
      LocalTrack track, TrackPublishOptions options);
  void unpublish_track(TrackSid sid);
  void publish_data(DataPacket packet);
  void update(ParticipantUpdate update);
};

class LocalTrack {
 public:
  static Result<LocalTrack> create_audio(
      std::string name, CodecFormat format, QueueLimits limits);
  static Result<LocalTrack> create_video(
      std::string name, CodecFormat format, QueueLimits limits);

  TrackSnapshot snapshot() const;
  SenderFeedback feedback() const;
  WriteStatus try_write(EncodedFrame frame);
  void set_muted(bool muted);
};

class RemoteTrackPublication {
 public:
  PublicationSnapshot snapshot() const;
  void set_subscribed(bool subscribed);
  void set_receive_options(ReceiveOptions options);
  std::optional<RemoteTrack> track() const;
};

class RemoteTrack {
 public:
  TrackSnapshot snapshot() const;
  ReadResult read(WaitOptions options);
  void request_keyframe();
};
```

### 5.1 返回值规则

| 接口类型 | 返回方式 | 错误表达 |
| --- | --- | --- |
| 连接 | `bool` | `false`，详细原因记录日志，连接状态反映失败 |
| 创建或发布业务对象 | `Result<T>` | 返回失败原因，不以空对象冒充成功 |
| 无返回值控制 | `void` | 失败日志；已有状态查询和相关状态事件反映实际结果 |
| 普通查询 | 数据值、句柄或 `optional` | 不存在使用明确的缺省/无效状态 |
| 高频媒体读写 | `WriteStatus` / `ReadResult` | 背压、未就绪、超时、关闭等专用状态 |

连接前 `local_participant()` 返回可检查的无效句柄；成员集合为空。
通过无效句柄执行操作时遵循同一错误规则，不触发未定义行为。

void 调用返回表示本次调用已经完成、失败或超时，不保证操作成功。
需要确认的应用通过状态查询检查；不新增通用的“操作完成事件”来替代完成回调。
尤其是 `publish_data()`，首版日志式接口不提供逐条成功确认能力。

错误日志包含操作、房间/轨道标识、失败类别和底层原因，不记录 token、密钥或媒体内容。
背压等预期高频状态直接返回，诊断日志限频。

日志实现统一包含 `support/logger.h`，使用其中的
`LOG_DEBUG(tag, msg)`、`LOG_INFO(tag, msg)`、`LOG_WARN(tag, msg)`、
`LOG_ERROR(tag, msg)` 和 `LOG_CRITICAL(tag, msg)`。消息参数使用流式拼接：

```cpp
#include "support/logger.h"

// 示意：reason 必须是已去除凭据等敏感内容的错误描述。
LOG_ERROR("Room", "connect failed: " << reason);
LOG_WARN("LocalTrack", "publish timed out, track_id=" << track_id);
```

复制的头文件保持原有宏定义和头文件保护符。宏的 tag 参数按模块填写，例如 Room、
LocalTrack、SignalSession。Android 使用系统日志，其他平台使用
`utoolkit::logging::Logger`；头文件仍需 `utoolkit/logging/logger.h` 的包含路径。
后续构建目标通过私有编译定义设置 `UT_LOG_TAG="LiveKit"`，覆盖原文件的 GraphFlow
默认 Android 标签。实现时单独接入 `graphflow-cpp/utoolkit/logging` 的 CMake 目标，
已有 `utoolkit_logging` 时复用，不重复添加，不依赖 GraphEngine 目标。

当前仅复制宏头文件并确定使用规范；客户端构建目标和日志库接线随实现阶段完成。

### 5.2 同步完成条件

| 操作 | 成功路径的返回条件 |
| --- | --- |
| connect | 信令和所需传输建立，初始成员与发布状态已写入模型 |
| publish_track | 服务端确认发布，编码帧发送端点就绪 |
| set_subscribed(true) | 对应接收轨道绑定完成，可读取编码帧；不等待首帧 |
| set_subscribed(false) | 本地接收绑定解除，必要取消订阅信令已提交 |
| unpublish_track | 本地发送绑定解除，必要取消发布信令已提交 |
| set_muted | 本地发送门控更新；已发布时完成静音信令提交 |
| set_receive_options | 接收需求完成本地校验和信令提交 |
| request_keyframe | 关键帧请求被本地传输接受或按限频规则合并 |
| publish_data | 数据被本地传输接受，不代表远端业务收到 |
| try_write | 立即返回入队结果，不等待网络发送 |
| read | 帧可用、超时、取消、暂停或终止时返回 |
| close | 内部任务和传输清理完成，所有被接受的等待操作已结算 |

没有服务端 ACK 的协议操作不能声明“服务端处理成功”。信令提交失败必须记录日志；
对于已执行的本地门控或解绑，状态查询如实反映本地结果，不伪造远端确认。

## 6. Observer 与线程模型

采用单个 `RoomObserver*` 注册槽，观察者由应用持有。

- 可在连接前注册，接收连接初期事件。
- 事件包括连接变化、成员进出和信息更新、发布/订阅变化、轨道失败、静音变化、
  重新发布和发送反馈变化。事件承载必要的对象句柄或不可变快照。
- 所有 Observer 通知在独立通知队列串行执行，不在网络线程或媒体读写线程调用。
- 先更新内部状态，再排队发送对应事件。同步操作返回不依赖 Observer 是否已经消费事件。
- 事件可能在同步调用返回之前或之后被消费，应用不能把二者视为固定顺序。
- `set_observer(nullptr)` 和替换操作等待已排队的旧观察者通知完成。
  旧观察者必须存活到注销或替换返回。
- 不提供 `wait_event()` 或事件拉取通道，也不提供每次操作的完成回调。

内部职责划分：

1. 控制队列串行管理房间、成员、轨道绑定和请求状态。
2. 网络/WebRTC 线程只产生内部事件，不调用应用 Observer。
3. 编码帧队列独立处理媒体数据，不把每一帧排入控制或通知队列。
4. 通知队列交付应用事件；外部调用线程等待内部条件变量或等价同步原语。

等待期间不持有共享状态锁，不持锁调用 Observer。
Observer 中允许只读快照查询；需要同步控制时转交应用工作线程。
不允许在 Observer 中执行阻塞控制、阻塞读帧、替换 Observer、close 或销毁 Room。
可检测的违规调用应立即拒绝并按返回类型报告/记录错误，避免等待自身造成死锁。
销毁仍必须由外部所有者保证，不能依赖日志恢复已开始的错误生命周期。

## 7. 超时、并发和关闭

同步 API 不意味着整个实现只有一个线程。

- `ConnectionOptions` 设置有限的连接超时和默认操作超时，均使用单调时钟。
- 读帧通过 `WaitOptions` 提供超时和取消；沿用项目的 C++17 取消兼容设施。
- 网络操作等待超时后退出，记录失败；内部请求标识防止迟到响应完成错误的请求。
- 创建发布/订阅失败或超时后需要清理部分建立的绑定；迟到服务端确认也必须补偿处理。
  回滚失败保留可定位状态和日志，由后续清理或 close 收敛。
- 同一对象的冲突操作串行处理；连续的订阅意图变更应取消被覆盖的等待，不能让旧结果覆盖新意图。
- close 可从另一个外部线程打断 connect、publish 和 read 的等待。
- close 幂等：关闭入口、取消请求、关闭媒体队列、停止传输、等待内部任务退出，
  最后排空通知；返回后不再交付本房间事件。
- 清理等待有内部期限和强制中止传输路径，不能因缺少网络响应无限等待。
  close 返回仍必须满足没有后台任务继续访问已释放资源的保证。

建议状态机：

```text
created → connecting → connected ⇄ reconnecting
              ↓            ↓            ↓
            failed       closing ←───────┘
              ↓            ↓
            closing →    closed
```

连接失败返回 false 并进入 failed；显式 close 后进入 closed。
created 状态也可直接关闭。failed/closed 实例不重新 connect，新的连接尝试创建新 Room。
已连接会话的自动重连仍在同一个 Room 内完成。

## 8. 编码帧契约与背压

发送和接收路径：

```text
应用编码器/已编码文件 → LocalTrack.try_write → 编码帧适配 → WebRTC → LiveKit
应用解码器/存储/转发 ← RemoteTrack.read     ← 编码帧接收 ← WebRTC ← LiveKit
```

输入是编码访问单元，不是包含 RTP 头的网络包。应用负责文件解复用和外部编码。
首版沿用现有端点已支持并经过验证的 codec 与码流约束；候选包括 Opus、VP8、H.264、H.265，
具体可用性通过构建能力和协商结果确认，不承诺所有平台和服务端均支持。

复用或抽取现有 `CodecFormat`、`EncodedBuffer`、`EncodedFrame`、`QueueLimits`、
`WriteStatus`、`ReadResult` 和 `SenderFeedback` 的语义：

- 编码格式包含 codec、时钟频率、声道、尺寸及 fmtp 等协商参数。
- H.264/H.265 沿用现有 Annex-B 访问单元约束，不默默接受其他封装格式。
- 帧包含编码数据、发送层标识、时间戳、时长、关键帧标记、绑定 epoch 和配置版本。
- 时间戳使用声明的时钟频率，不将微秒、毫秒和 RTP ticks 混用。
- 缓冲区持有数据所有权，跨线程引用有明确有效期。
- 发送前应用从 Track 快照/反馈取得当前 epoch，旧 epoch 帧返回 `stale_epoch`。
- 每轨和房间总量均限制帧数/字节预算，限制队列最大年龄；预算覆盖底层持有的帧。
- 写入保留 `accepted`、`backpressure`、`keyframe_required`、`stale_epoch`、
  `not_ready`、`closed`、`invalid_frame` 等可判断状态。
- 发布前或重连尚未就绪时不接受无限预缓存。
- 视频丢帧不能破坏后续解码依赖；必要时丢弃失效帧并等待新的关键帧。
- 首版每个接收轨道只允许一个活动读者；多个消费方由应用自行分发。

发送反馈通过 `feedback()` 查询，并通过 Observer 通知变化，包括目标码率、
关键帧需求和发送层启停。应用将反馈交给自己的编码器；SDK 不代替应用重新编码。
通知合并同类反馈并保留最新状态及必要计数，不逐帧回调。

静音只控制本地发送及相应信令，不停止应用编码器；取消发布关闭发送绑定并清空旧队列。
重新发布或恢复发送时通过 epoch、配置版本及媒体代次隔离旧数据，需要时要求关键帧。

## 9. 所有权与重连

- Room 独占 RoomSession 的生命周期。
- Participant、Track 和 Publication 句柄持有各自状态，对会话使用弱引用，避免强引用环。
- Room 关闭后保留句柄可查询最终状态，不能继续控制会话或收发媒体。
- 本地 Track/Publication 的对象身份可跨自动重连保留，服务端 SID 作为可更新绑定。
- 全量重连重新发布成功后发出包含旧/新 SID 的事件，并推进绑定 epoch。
- 远端按 identity 与 publication 信息协调恢复；远端轨道消失时将旧句柄标记为终止，
  不按名称误绑定到另一条新轨道。
- 旧连接的事件、帧和请求响应必须经过会话代次校验。
- 网络恢复期间保持静音、订阅意图和接收需求；未恢复的轨道显式处于 suspended 或 failed。

事件队列同样有界。可合并的状态/反馈事件采用合并策略；关键事件不能静默丢失，
容量耗尽时通过可观测的失败状态和日志终止或降级相应会话，具体策略在实现前以测试固定。

## 10. 底层复用策略

当前 NativePeer 和 SignalSession 仍依赖旧 RTC 的类型、事件或恢复状态。
不能假设直接链接它们即可得到完全独立的新后端。

| 部分 | 策略 |
| --- | --- |
| Protobuf、WebSocket、WebRTC、任务队列和显式错误基础类型 | 复用 |
| NativePeer、TransportRuntime、编码帧发送与接收组件 | 抽取中立接口/类型并保留旧实现适配 |
| 信令编解码、连接、恢复策略 | 拆分可复用机制；房间恢复协调由新 RoomSession 管理 |
| Room、Participant、Track、Publication 对象与状态机 | 新实现 |
| RPC、数据流、加密等扩展 | 后续按功能拆分服务内核后接入 |

公共同步调用不通过“调用旧 RoomEngine 后阻塞等待完成回调”实现。
底层网络库的异步通知仍可用于内部机制，最终通过新会话自己的状态机和等待条件收敛。
公共 API 不泄漏这些回调。

## 11. 实施阶段与验收

| 阶段 | 交付 | 主要验收 |
| --- | --- | --- |
| 1：接口与依赖边界 | 公共头文件、独立目标、中立传输边界、RoomSession 骨架 | 头文件独立编译；关闭 GraphEngine/媒体组件后可构建；Fake 后端可注入 |
| 2：房间与事件 | 同步 connect/close、成员与发布模型、Observer、基础数据消息 | 鉴权失败/超时、初始状态、事件顺序、Observer 替换、并发关闭 |
| 3：编码媒体闭环 | 音视频发布订阅、编码帧队列、反馈、编码版 basic_room 示例 | 两端收发、背压、关键帧、静音、取消发布和多轨隔离 |
| 4：恢复与扩展 | 自动重连、重新发布、订阅恢复；再按需接入 RPC/流/加密 | SID/epoch 更新、迟到响应、恢复后有效媒体、反复连接关闭无资源残留 |

首个可用版本覆盖阶段 1～3，可通过公共同步接口完成连接、数据消息、编码音视频发布订阅和关闭。
该里程碑不宣称完整重连或与 Rust SDK 全功能对等；稳定交付需完成阶段 4 中的恢复验证。

验证方式：

- Fake 信令/传输单元测试：确定性模拟失败、延迟确认、超时、关闭竞争和订阅意图切换。
- 真实 LiveKit 服务集成测试：两个客户端完成编码音视频和数据消息收发。
- 使用固定编码样本或测试专用编码器供帧，接收端通过测试解码器检查有效性、时序和持续收发。
  不以“收到字节”替代完整媒体有效性验证。
- 重复创建、连接、发布、关闭，检查线程退出、回调注销、队列清空及内部状态销毁。
- 验证无 GraphEngine 构建、C++17、受影响目标的无异常编译和新文件格式。
- 不操作用户桌面；自动化结果与真实设备人工验收分开记录。

本文所述客户端尚未实现，未执行上述验收。已完成的准备工作是将日志宏头文件
原样复制到 SDK，并核对源文件与副本的 SHA-256 一致。

## 12. 参考代码

- [现有 RoomEngine 公共接口](../include/livekit/rtc/room_engine.hpp)
- [编码轨道端点](../include/livekit/rtc/track_endpoint.hpp)
- [编码帧、队列、状态和反馈类型](../include/livekit/rtc/types.hpp)
- [现有 RoomEngine 与媒体层边界](room-engine-media-composition.md)
- [NativePeer](../src/backends/webrtc/native_peer.hpp)
- [SignalSession](../src/backends/rtc/livekit/signal_session.hpp)
- [SDK 日志宏](../src/support/logger.h)（复制自 `graphflow-cpp/utils/logger.h`）
- [项目代码规范](../../docs/development/coding-standard.md)
- Rust 参考：本机 `D:/workspace/rust-sdks/examples/basic_room/src/main.rs`、
  `livekit/src/room/mod.rs` 及其 participant、track、publication 模块。
