# 编码媒体 RTC API

新实现的媒体交接单位是**拥有数据的编码访问单元**。房间后端负责 LiveKit 信令、发布订阅、WebRTC 封包/解包及网络传输；采集、实际编码、实际解码、设备播放和渲染属于上层媒体图。

## 选择入口

| 场景 | 头文件 | CMake target |
| --- | --- | --- |
| 配置采集、编解码、预览与输出，由媒体引擎构图 | `livekit/rtc/media_engine.hpp` | `LiveKit::RTCMediaEngine` |
| 已有自己的调度/流水线，仅需房间和收发端点 | `livekit/rtc/livekit.hpp` | `LiveKit::RTCLiveKit` |
| 由 Engine 创建发布、订阅节点及 GraphEngine 流水线 | `livekit/rtc/engine.hpp` | `LiveKit::RTCEngine` |
| 在自己的 GraphEngine 中组合 RTC 节点 | `livekit/rtc/publish_node.hpp`、`subscribe_node.hpp` | `LiveKit::RTCNodes` |
| 在 engine 图中使用实际视频编解码 | `livekit/rtc/video_nodes.hpp` | `LiveKit::RTCMedia` |
| 在 engine 图中使用实际 Opus 编解码 | `livekit/rtc/audio_nodes.hpp` | `LiveKit::RTCMedia` |
| 生成、采集、文件或应用供帧输入节点 | `livekit/rtc/input_node.hpp` | `LiveKit::RTCInputs` |
| 独立输入队列、设备枚举与媒体探测 | `livekit/media.hpp`、`video_source.hpp`、`audio_source.hpp`、`encoded_media.hpp` | `LiveKit::MediaInputs` |

命名空间为 `livekit::sdk::rtc`；Engine 位于其 `engine` 子命名空间。节点位于 `nodes` 子命名空间。

每个后端是一段房间会话。先 `join()`，最后 `leave()`；再次入会创建新的后端。发布和订阅分别拥有轨道句柄，多轨共享同一个后端。关闭轨道只解绑该轨道。

## 后端接收

```cpp
#include <livekit/rtc/livekit.hpp>
using namespace livekit::sdk::rtc;
using namespace std::chrono_literals;

auto room = create_livekit_room();
RoomOptions options;
options.url = url;
options.token = token;
room->join(options).get();

// 在应用控制循环中读取发现事件。
auto event = room->next_event(100ms);
if (event && event->kind == RoomEventKind::remote_track_added) {
  auto descriptor = *event->descriptor;
  descriptor.id = "camera-input"; // 本地路由 ID，独立于服务端 SID
  auto track = room->subscribe(descriptor).get();
  auto received = track->read(100ms);
  if (received.status == ReadStatus::frame) {
    auto frame = std::move(*received.frame);
    // frame.buffer.bytes() 是完整 Opus/VP8/H26x 编码数据。
    // frame 可以移交给应用自己的有界队列。
  }
  track->close().get();
}
room->leave().get();
```

发现事件里的 `remote_sid` 是订阅选择器，不能省略或当作可重用的本地 ID。相同远端 SID 只创建一个后端订阅；需要多个消费者时，在媒体图内分流。

`read()` 还会明确返回超时、取消、暂停、关闭或失败。应用不在 WebRTC 回调线程上处理媒体。缓冲区保持不可变；复制帧会共享载荷所有权。

### 独立订阅需求

`TrackDescriptor::receive_demand` 是初始需求。订阅成功后，使用 `SubscribedTrack::set_demand`，或通过 backend/engine 的 `set_receive_demand(id, demand)` 更新。每次完整替换配置，省略字段恢复默认值。

```cpp
ReceiveDemand demand;
demand.width = 640; demand.height = 360;
demand.fps = 30;
demand.priority = 1; // 1 最高；0 由服务器安排。
track->set_demand(demand).get();
demand.disabled = true;
track->set_demand(demand).get(); // 只暂停这条订阅的服务器转发。
```

宽高都为零时使用 `quality`（默认 high）；指定尺寸时宽高必须都大于零。音频只支持 disabled 和 priority。尺寸上限 16384、帧率上限 240。服务器按可用层及带宽选择转发，目标尺寸/帧率不是输出保证，也不触发转码。该控制不停止本地解码/渲染节点；已有缓冲和网络在途数据仍可交付，`read()` 不因此返回 suspended。

future 成功表示原生适配器已提交信令，协议没有该操作的服务器 ACK。`snapshot().receive_demand` 保存最后成功完成的本地提交值，descriptor 保留初始配置；重入按 snapshot 恢复。取消、超时或断线后的未知结果不替换该恢复值，服务器可能已收到请求，应用可重新提交所需配置。控制共用 128 个待处理操作上限，支持 `ControlOptions`；旧订阅句柄不能控制同名替代订阅。

## 房间与成员快照

`room->snapshot().session` 是当前信令会话的 `shared_ptr<const SessionInfo>`。它包含房间 SID、名称、元数据和录制标记，本地成员，以及按成员 SID 索引的远端成员。每个成员包含身份、显示名称、元数据、属性、发布轨道、静音标记、可选权限、发言状态与连接质量。未收到权限字段表示未知，不能直接当作禁止。

快照中的发布轨道是服务端公告，包含尚不支持的 codec；它不表示已创建订阅或解码器。实际可订阅的编码端点仍通过 `remote_track_added` 发现。本地发布公告不会触发远端发现。

`session_changed` 是可合并的状态变更提示，收到后读取最新快照；它不承诺逐条回放所有发言/质量变化。持有旧快照不会看到内容被修改，但它只代表历史状态。中断后当前快照清空，新 JoinResponse 到达后安装新 epoch 的快照，可能早于媒体连接完成；终止后清空。不能拿旧 epoch 的 SID 操作新会话。低版本成员更新和已离开 SID 的延迟更新会被拒绝。

成员权限字段用于状态观察，权限快照并不替代服务端授权检查。本地轨道静音控制见下文。

### 发布方订阅权限

`RoomOptions::subscription_permissions` 可在入会时设置发布方的订阅允许列表。原生适配器先提交策略，再接受媒体发布；通过稳定的本地 publication ID 指定允许的轨道，发布确认及每次重新入会后重新解析服务端 SID。未发布或已经移除的 ID 不会扩大授权范围。

```cpp
SubscriptionPermissions policy;
policy.all_participants = false;
policy.participants = {{"viewer", false, {"camera-main", "screen-main"}}};
options.subscription_permissions = policy;
room->join(options).get();
// 随后发布 camera-main / screen-main；无需预先知道服务端 SID。
```

运行中用 `room->set_subscription_permissions(policy)` 完整替换策略。`all_participants=true` 允许全部成员，不能同时提供成员列表；`all_tracks=true` 允许指定 identity 的全部轨道，不能再指定 track_ids。false 加空成员列表拒绝全部订阅。最多 64 个成员、每个成员 128 个轨道 ID、总文本 32,000 字节，拒绝重复 identity/ID 和非法 UTF-8。

成功 future 表示信令已提交，协议没有此请求的服务器 ACK。原生控制循环保留最新派发的策略并在重入时恢复；派发后取消、超时或发送中断可能返回 `outcome_unknown`，新策略仍可能生效并被恢复。需要改变意图时重新提交完整策略。策略由 SFU 执行，不能撤回接收端已经持有的媒体。

订阅方通过 `subscription_permission_changed` 事件的 `permission` 读取 publisher participant SID、track SID 和 allowed。该通知可以在订阅尚未绑定时到达；等待中的 subscribe future 仍受 operation_timeout 限制，授权及时恢复可继续完成，超时后需要重新 subscribe。运行中撤销与重新授权由服务器驱动转发变化；已绑定句柄不因权限提示自动销毁。权限通知带房间 epoch，旧会话事件不会混入当前会话。

### 修改自己的成员信息

使用 `RoomSession::update_local_participant(ParticipantUpdate, ControlOptions)` 更新自己的名称、元数据和属性。必须开启 `RoomOptions::signal_protocol = 15` 或适配器支持的更高版本；future 只在服务端 ACK 后成功，不以本地写入信令队列代替确认。服务器拒绝权限时返回 `SDKError`，错误码为 `permission_denied`。

```cpp
livekit::sdk::rtc::ParticipantUpdate update;
update.name = "演示者";
update.metadata = R"({"role":"presenter"})";
update.attributes = {{"language", "zh"}, {"obsolete-key", ""}};
room->update_local_participant(std::move(update)).get();
```

空名称/元数据遵循 LiveKit 线协议语义，表示保持原值；空属性值删除该键，未包含的键保持原值。ACK 和本地/远端成员快照是独立观测，应用通过 `session_changed` 读取随后更新的状态。

默认超时使用房间的 `operation_timeout`；`ControlOptions` 可指定不超过五分钟的超时与停止令牌。请求在派发前取消/过期会明确失败；派发后取消、超时或中断可能已经改变服务器状态，返回 `outcome_unknown`，不会自动重放。重连后的调用使用新 epoch 和未复用的请求 ID，迟到 ACK 不能完成新操作。每次更新最多 64 KiB，最多 128 个待确认操作。更新不会自动持久化为下一次入会的身份配置。

## 发布与多流

### 上层输入节点

`nodes::MediaInputNode` 在发布图中输出 `VideoFrameEvent`、`AudioFrameEvent` 或 `EncodedFrameEvent`，不持有 Room、PeerConnection 或 RTC 编码器。原始帧连接独立 codec 节点，编码帧直接连接发布节点。视频发送示例使用这个节点，见 `examples/video_source/main.cpp`。

`MediaInputOptions` 必须恰好选择一种输入：`generated=true` 生成测试画面/音调、`input` 指定文件/URL、`capture` 指定 camera/screen，或设置 `video_source` / `audio_source` / `encoded_source`。文件/URL 在上层启动外部 FFmpeg 解码；`ffmpeg` 指定可执行文件，`loop` 只对文件输入有效。输入探测 `probe_media()` 和应用输入队列已移到独立 MediaInputs 库，不需要旧 SDK。

图中需要将发布节点的 `SenderFeedbackEvent` 同时连接到输入节点和编码节点。factory 返回的 `activate` 调用输入节点 `activate()`，`quiesce` 先停止输入再停止编码器，`poll` 调用两者的 `throw_if_failed()`。这保证下游就绪后才开始生产，关闭时先等待源退出，再拆除下游。

应用视频源使用固定 I420 尺寸，须匹配 descriptor；设备视频会转换为正向 I420 并缩放至发布尺寸。应用 PCM 以原始采样率交付完整 10 ms 块，使用 48 kHz 媒体时间戳，重采样由后续 Opus 节点负责。编码源保留载荷并展开 32 位 RTP 时间戳，Opus 透传无需生成 PCM 或依赖音频编码时钟。每个应用源只能被一个活动输入节点消费，`quiesce()` 会释放这一占用，即使调用方还持有节点用于读取统计。

输入节点根据会话 epoch 和静音代次清理应用输入缓冲；源代次防止旧数据改贴新会话标签，未来采集时间不会提前交付。编解码节点也拒绝切换前采集、切换后迟到的原始输入。已经进入下游回调的数据不能撤回。文件输入在重新绑定后继续读取，不自动从头重开；跨轨同步和设备播放仍需要上层进一步实现。

`snapshot()` 报告输出/丢弃计数、运行状态、结束与错误。自然 EOF 不会自动取消发布；engine/应用决定何时移除轨道。下游回调须保持非阻塞；阻塞的文件管道和子进程可以由 `quiesce()` 终止。这些能力的自动化测试使用内存帧、测试子进程和真实 FFmpeg 文件，没有操作摄像头、桌面或音频设备。

每条流使用不同 `TrackDescriptor::id`，各自调用 `publish()`。编码器生产 `EncodedFrame`，使用该发布句柄当前 `snapshot().epoch` 填写 `epoch`，通过 `try_write()` 提交。视频使用 90 kHz 时钟，Opus 使用 48 kHz 时钟；时间戳在一条流的同一 epoch 内递增。H.264/H.265 使用 Annex-B 访问单元。

通过 `TrackDescriptor::source` 显式设置 `PublicationSource::camera`、`microphone`、`screen` 或 `screen_audio`；它与 `source_id` 的来源身份不同。默认 unknown 按原值发送，不推断成摄像头或麦克风。发布时检查来源和媒体类型匹配，远端发现和成员快照保留该字段，便于区分同一成员的摄像头/共享屏幕与按来源授权。

`try_write()` 区分接受、背压、需要关键帧、旧 epoch、未就绪、关闭和无效帧。它表示有界队列准入，不是远端接收确认。重连或丢帧后，视频生产方应根据反馈生成新的关键帧。

`unpublish(id)` / `unsubscribe(id)` 也可取消等待确认的操作。成功完成的关闭 future 表示本地传输已解绑；服务端协商可以稍后完成。

当前原生适配器支持多条独立音视频轨道，每轨一个 encoding。单轨 simulcast/SVC 暂未实现，会明确拒绝。多轨与单轨多编码层是不同能力。

### 轨道静音和恢复

发布前可设置 `TrackDescriptor::muted`，通过 `AddTrackRequest.muted` 以静音状态发布。发布后使用 `PublishedTrack::set_muted(bool)`、`RoomSession::set_muted(id, bool)` 或 `RtcEngine::set_muted(id, bool)`；它们支持 `ControlOptions`。静音保留轨道、订阅和其他轨道的发送，不等同于取消发布。当前状态读取 `TrackSnapshot::muted`，描述符保留初始配置。

静音立即关闭端点输入准入并清空待发帧，随后在原生发送器中使已排队的旧代次失效。`try_write()` 在静音期间返回 `not_ready`。恢复后，视频必须从关键帧开始；本地静音意图会作为新发布配置随完整重入恢复。已越过原生发送检查或进入网络协议栈的包、重传及 SFU 填充帧无法撤回，不能用“接收端完全没有 RTP”判断静音是否生效。

静音 future 成功表示本地原生门控已应用并且信令已提交，**不表示服务端 ACK**，当前 MuteTrackRequest 协议没有独立请求 ID。房间快照里的本地静音标记也包含本地提交状态；远端快照来自服务端公告。超时/取消后的结果可能未知，不自动重放；已关闭的旧轨道句柄不能控制同名新轨道。服务端的 mute 命令会更新原生门控及对应端点。

`SenderFeedback::media_generation` 在静音边界变化，即便静音和恢复都发生在两次 engine `poll()` 之间也不会丢失边界。实际音视频 codec 节点据此清理旧输入和编码积累，停止接收非活动流的原始帧，恢复时重建正确的编码连续性。外部生产者同样应清理自己的缓存，并尽量提供 `EncodedFrame::captured`；端点会拒绝采集时间早于最近静音边界的旧帧。Engine 仍需持续 `poll()` 以把反馈送达媒体节点。

## Engine 和节点（高级图扩展）

普通采集、编解码、预览、播放和录制会话使用 [RtcMediaEngine 配置式 API](rtc-media-engine.md)，Session 不创建 GraphNode。下述 factory 与节点接口供高级图扩展使用，媒体引擎内部也通过这些接口构建每轨图。

`engine::create_livekit_engine()` 创建真实 LiveKit 后端。`publish()` 为轨道创建发布 sink 图，`subscribe(descriptor, consumer)` 创建订阅 source 图，consumer 接收 `nodes::EncodedFrameEvent`。

Engine 的创建图、关闭图和 `poll()` 在同一应用控制线程执行。应用必须持续调用 `poll()`，它会完成异步创建图、分发接收帧和反馈；不能在该线程直接阻塞等待发布/订阅 future 而停止轮询。`write()` 可由生产媒体的线程调用。

各轨道具有独立队列和图生命周期。房间总预算随载荷在端点、原生发送、原生接收队列间转交，不重复扣额度；读出到应用后，应用自行负责后续内存限制。此预算统计 SDK 管理的待处理访问单元，不包含网络协议栈的 RTP 重传缓存、操作系统 socket 缓冲区或应用保留的帧。

`TrackDescriptor::queue` 的帧数和字节额度同时覆盖该端点与已绑定原生收发队列；发送移交给原生层后仍占额度，直到原生处理完成。接收端也在交给应用时释放。`TrackSnapshot::queued_*` 是端点当前队列，`outstanding_*` 包含原生层仍持有的访问单元。尚未绑定的原生接收暂存使用房间预算，建立订阅绑定时清空并安装对应轨道额度。

完整示例见 `examples/room_client/main.cpp`。发布模式发送两路预编码 Opus 静音，订阅模式通过 GraphEngine 接收编码帧，均无需实际编解码或设备。

### 组合媒体图

`publish_graph(descriptor, factory)` 和 `subscribe_graph(descriptor, factory)` 在端点确认后、engine 的 `poll()` 中调用 factory。factory 收到对应的 RTC 节点，返回 `TrackGraphSpec`：新建的媒体节点、类型化连接、可选的 `activate`、`quiesce` 和 `poll`。它们与 RTC 节点属于同一张每轨 GraphEngine 图。

- `activate` 在所有节点启动、初始发送反馈送达之后执行，适合启动采集线程。
- `quiesce` 必须不抛异常，并在返回前关闭准入、等待生产者/编解码 worker 结束。engine 在停止图内节点之前调用它，部分激活失败也会调用。
- `poll` 在 engine 控制线程运行，工作量必须有界；耗时编解码由节点自己的有界 worker 执行。
- factory 不能复用其他图已初始化的节点；每条流的编码器、解码器和输入队列独立。控制回调不能重入 engine 的图生命周期操作。

可选的 `nodes::VideoCodecNode` 在编码模式下接收拥有 I420 像素的 `VideoFrameEvent` 和 `SenderFeedbackEvent`，输出 `EncodedFrameEvent`；解码模式反向转换，并通过 `KeyframeRequestEvent` 请求恢复帧。将后者连回订阅节点即可触发限频 PLI。媒体节点不创建 Room 或 PeerConnection。

默认每轨编解码准入队列最多 2 帧、32 MiB、250 ms，超限后丢弃旧队列并恢复关键帧；编解码库内部缓冲不计入此准入队列。每个节点拥有独立 worker。下游回调在该 worker 上执行，耗时渲染或业务处理应转交给应用有界队列。`snapshot()` 提供准入、完成、丢弃、队列及错误信息；将 `throw_if_failed()` 接入图的 `poll` 可让 engine 隔离失败轨道。

`VideoFrameEvent::timestamp` 保留 90 kHz 媒体时间，`epoch` 在原始帧准入时确定。重连时不能把尚未编码的旧帧改成新 epoch。发布端需使用 `quiesce` 先停止输入生产者，再调用编码节点的 `quiesce()`；接收端同样等待解码节点结束后才释放输出消费者。完整示例见 `examples/video_source/main.cpp` 和 `examples/video_stream/main.cpp`。

构建 `LiveKit::RTCMedia` 使用 `LIVEKIT_BUILD_RTC_MEDIA=ON`，并开启 `RTC_NATIVE` 和 `RTC_GRAPH_NODES`。它不依赖旧 SDK facade。VP8 使用内置软件 codec；H.264/H.265 的实际可用性取决于所选构建，FFmpeg 扩展由 `LIVEKIT_RTC_FFMPEG_CODECS` 控制。仅需要编码数据收发时关闭 RTC_MEDIA。

### 原始音频与 Opus 节点

`nodes::AudioCodecNode` 编码模式接收 `AudioFrameEvent` 与 `SenderFeedbackEvent`，输出 `EncodedFrameEvent`；解码模式反向转换。它在独立有界 worker 上运行实际 Opus 编解码，不拥有房间或 PeerConnection。

输入是恰好 10 ms 的单/双声道交错 PCM16 或 float32，采样率为 8–96 kHz 且可被 100 整除，声道数与轨道描述一致。例如 44.1 kHz 单声道每次输入 441 个样本。节点重采样至 48 kHz，可聚合为 10/20/40/60 ms 的 Opus 包，默认 20 ms。解码输出为 48 kHz PCM16，长度由包内实际时长决定，支持 2.5–120 ms。

`AudioFrameEvent::timestamp` 始终使用 48 kHz 时钟，10 ms 输入递增 480，与输入 PCM 采样率无关。`epoch` 在原始数据准入时固定。旧 epoch、过载、超时或时间戳缺口会丢弃/重置相关积累，避免跨代次拼包。解码输出保留时间戳缺口，通过 `discontinuity` 标记边界；此节点不提供 NetEq、丢包补偿、混音、播放节拍或音画同步。

默认准入队列上限为 25 项、256 KiB、250 ms。解码消费者在 codec worker 上执行，业务处理应移交应用有界队列。`audio_stream` 示例使用最多 128 块、512 KiB 的应用队列；`audio_source` 示例按 10 ms 节拍向 `PcmAudioSource` 提交 44.1 kHz 浮点正弦波，由 `MediaInputNode` 接入编码器。图关闭时使用 `quiesce` 等待编解码 worker，使用 `poll` 调用 `throw_if_failed()` 传播轨道错误。

### 输入节点与麦克风

`LiveKit::MediaInputs` 提供独立输入队列、设备采集及文件探测；`LiveKit::RTCInputs` 将这些输入封装为 `nodes::MediaInputNode`。在 `MediaInputOptions` 中恰好选择一种输入：生成源、文件/URL、摄像头/屏幕、应用 I420/PCM/编码源或 `microphone`。把 RTC 发布节点的 `SenderFeedbackEvent` 同时连接到输入节点和编码节点，输入节点在图激活且收到首次绑定反馈后生产媒体。

```cpp
auto microphone = std::make_shared<livekit::sdk::MicrophoneSession>();
nodes::MediaInputOptions input;
input.microphone = microphone;
// audio_descriptor: audio/Opus/48000/mono，source 显式设为 microphone。
auto source = std::make_shared<nodes::MediaInputNode>("microphone", audio_descriptor, input);
// source --AudioFrameEvent--> encoder --EncodedFrameEvent--> publish_sink
```

`MicrophoneSession` 不依赖 Room 或 PeerConnection。原生设备由独立控制线程创建/启动/停止，在设备回调中转换声道并重采样为 48 kHz PCM16，写入默认 100 ms 的有界源队列；GraphEngine 和用户回调由输入节点的工作线程调用。队列满时丢块，不等待容量。每个会话只能有一个输入读者，需要多路使用时在输入节点之后扇出。

图的 `quiesce` 先停止输入节点，再停止编码器。输入退出会等待设备回调停止并释放源读者，保留的会话可在后续图中复用。房间重连、静音代次切换清空源队列；设备回调携带预期源代次原子提交，迟到回调不能改贴新代次。回调抖动按 10 ms 采样节拍处理；长暂停保留时间缺口，累计快时钟偏差通过丢块限制。这不是完整的自适应时钟恢复。

`audio_source <url> <token> --microphone` 使用实际麦克风；省略该参数使用应用正弦波。示例链接 `LiveKit::RTCMediaEngine`，通过 MediaInput 的 microphone 配置由引擎创建设备会话；构造本身不会打开设备。

### 多轨共享音频输出

`LiveKit::RTCOutputs` 提供 `AudioOutputSession` 和每轨 `nodes::AudioOutputNode`：

```cpp
auto device = std::make_shared<AudioOutputSession>(); // 多条订阅共用
auto output = std::make_shared<nodes::AudioOutputNode>("speaker", descriptor.id, device);
// subscribe_source --EncodedFrameEvent--> decoder --AudioFrameEvent--> output
```

每个输出节点在图启动时打开独立轨道队列，第一条轨道启动物理设备，最后一条关闭后停止设备。图的 `quiesce` 先等待解码器退出，再调用输出节点的 `quiesce()`。关闭一条订阅只清理该轨道；旧输出句柄不能影响后来创建的同名轨道。将 `output->throw_if_failed()` 放入图的 `poll`，设备错误才会传到 engine。

输出接受 48 kHz PCM16 单/双声道，帧长度 2.5–120 ms。默认最多 32 轨、每轨 250 ms 队列、首帧预缓冲 40 ms。设备按 10 ms 拉取，共享混音后饱和到 PCM16，再适配设备的采样率/声道数。渲染回调遇到队列锁竞争输出静音并推进设备时钟，不阻塞等待生产者。每轨媒体时间戳独立锚定设备时钟；缺口填静音，重叠/迟到帧丢弃，队列超限或超过延迟上限的时间跳变只清理并重新锚定对应轨道。

`set_gain(0..4)`、`set_muted(bool)` 控制单轨输出；静音切换清队列并拒绝切换前采集的迟到帧。`snapshot()` 提供排队、丢弃、静音和实际混音样本计数。这里的混音完成不表示扬声器已经播放该样本，设备内部仍有缓冲。

`audio_stream --playout` 将所有发现的音频订阅接到同一设备会话；不带该参数仍只读取 PCM。示例链接 `LiveKit::RTCMediaEngine`，通过 MediaOutput 配置播放。输出不提供 NetEq/PLC、发送端跨轨音画同步或 AEC/APM；这些仍须在上层媒体会话中补齐。自动化使用虚拟设备驱动及真实原生回调转换器，不打开麦克风/扬声器，不能替代设备人工验收。

`LiveKit::RTCOutputs` 同时提供 `nodes::RecordingNode(name, directory, track_id, kind)`，可把解码器的 `AudioFrameEvent`/`VideoFrameEvent` 接到独立 WAV/Y4M 录制分支。默认准入上限为 32 帧、64 MiB；磁盘写入在节点 worker 上执行，溢出或文件错误通过 `throw_if_failed()` 报告。关闭时先停止解码器，再 `recording->quiesce()` 排空写入并完成文件头，之后检查错误。路径独占创建，不覆盖已有文件；epoch/格式变化会分段。此原始录制格式不保存 RTP 时间轴，也不提供跨轨同步。

## 数据服务与加密

`room->services()` 返回共享会话的数据服务句柄，提供可靠/不可靠消息、DTMF、转写、RPC、文本/文件流和 DataTrack。这些功能使用后端原生数据通道，不依赖编解码或 GraphEngine。用 `next_event()` 拉取数据事件；RPC/流处理器运行在服务执行器上，不运行在原生网络回调中。

用户数据发送/接收队列各限制为 256 条、4 MiB；单次编码前的数据包最多 15,000 字节，大内容使用流或 DataTrack。发送 future 表示本地提交，不表示对端业务已消费。会话关闭会拒绝待处理发送；断线取消当前 RPC/流请求并重建 DataTrack 绑定，处理器注册保留。保留的服务句柄不会悬空，但会话关闭后不能继续发送。

在 `RoomOptions::encryption` 提供 KeyProvider 可开启数据 E2EE，设置 `media=true` 还会开启 Opus/H.264/H.265 媒体 E2EE；当前不支持 VP8 媒体加密。媒体在发送封包前加密、接收编码帧交付前解密。认证失败的媒体不进入应用，状态通过 `encryption_state_changed` 报告。DTMF/转写不属于当前协议加密助手支持的载荷类型。配置密钥不替代应用自己的密钥分发。

## 接收语义和当前边界

`RoomOptions::stats_interval` 可开启原生传输统计（0 关闭，200–60000 ms）。`RoomEventKind::statistics` 的 `TransportStatistics` 包含 publisher/subscriber 目标、原生时间戳和 W3C JSON 数组；不含候选地址或证书记录。统计回调不调用应用，报告按目标合并，队列容量不足时让位于控制事件，旧 epoch 的迟到报告被过滤。实际上层 codec 指标仍通过各节点的 `snapshot()` 获取。

- 接收输出是解包后的访问单元，不是原始 RTP 包。应用不能据此假定原始 SSRC、包边界和绝对 RTP 起点不变。
- 视频保留相对媒体时钟；原生发送/SFU 可重写 RTP 起点。时间戳不直接等同于墙上时间，跨轨播放同步还需上层同步机制。
- 当前接收适配器丢弃重复/倒退的时间戳；视频丢弃依赖链后请求关键帧。它尚未提供完整的应用层乱序窗口和播放同步 API。
- SFU 可以注入合法编码帧，例如 LiveKit 退订时发送 VP8 清屏帧，或在发布端静音后发送 Opus 静音帧。因此“收到编码帧”不保证它一定来自应用提交的原始帧序列。
- H26x 解包可能规范化 Annex-B 起始码，原生 H.264 还可能改写 SPS VUI；不要依赖任意码流逐字节完全不变。
- 数据消息、RPC、数据流、DataTrack 和 E2EE 已接入新 backend。逐轨权限和产品调用方已迁移，旧 Room 路径已删除。单 PC、信令 resume/无缝迁房仍不支持；断线执行完整重新入会。

验证使用无窗口单元测试、原生 ICE/DTLS/SRTP 回环和独立本机 SFU 进程；这些不等同于真实设备人工验收。


## 发布需求和诊断映射

单层视频发布可设置 `TrackDescriptor::follow_subscriber_demand=true`。backend 处理服务器聚合订阅需求并通过 `SenderFeedback` 的 `active` 通知上层。OFF 使原生编码载荷队列失效，ON 要求关键帧；本地静音与需求门控独立，ON 不解除静音。`MediaInputNode` 会暂停生成/应用实时输入，设备输入停止后按需求重新启动；有限文件和 URL 输入不接受此策略。当前不实现 simulcast/SVC 或备用 codec 切换。

`RoomOptions::media_transport` 可选择 `MediaTransport::automatic`、`udp_only`、`tcp_only`。显式 UDP/TCP 模式禁用 TURN，配置作用于 publisher/subscriber 两个原生 PeerConnection。

`TransportStatistics::publication_tracks` 将 publisher 原生 `trackIdentifier` 映射到该报告对应会话的服务器 SID，用于按轨聚合 RTP 字节和帧计数。subscriber 报告不提供此映射；应用不能用其空映射覆盖 publisher 的映射。统计和实际 codec 节点计数有各自的采样时钟。

### 服务器移除轨道

服务器主动撤销发布会使端点进入 `closed`，清空本地发布意图、停止发送并从 engine 移除该轨 GraphEngine。`RoomEventKind::track_removed` 携带本地 `track_id`、descriptor 和原因；正常移除不进入 `RtcEngine::next_failure()`。应用应移除对应预览/业务登记。`track_failed` 和图 failure 队列仍用于操作或处理错误。其他轨及共享房间继续运行，重连不会自动重新发布已撤销的轨道。
