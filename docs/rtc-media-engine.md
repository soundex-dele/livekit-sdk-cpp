> 历史文档：旧 RtcEngine / RoomEngine 及底层实现已移除。本文仅记录当时设计或验证，不代表当前 API；参见 [迁移说明](legacy-engine-removal.md)。

# 统一 RTC 引擎与配置式媒体 API

本文按 2026-11-02 当前源码整理。公共声明以 [engine.hpp](../include/livekit/rtc/engine.hpp)、[media_config.hpp](../include/livekit/rtc/media_config.hpp) 为准；线程与完成语义见 [Completion / Observer 契约](rtc-simple-observer.md)。本文描述已实现接口，不将后续 API 简化建议视为已有能力。

## 职责与所有权

`RtcEngine` 是普通 RTC 媒体会话的统一入口，负责把房间端点与媒体图组成一次完整操作：请求端点、构图、启动、保存轨道、失败回滚、停止和解绑。客户端不需要协调 GraphEngine 与传输端点的生命周期。

| 模块 | 当前职责 | 所有权与边界 |
| --- | --- | --- |
| `RtcEngine` | 发布订阅编排、轨道目录、回滚和关闭 | 直接持有成员，无 PImpl；发布和订阅分别存入两个 map |
| `RoomEngine` | 会话、协议请求/事件、编码端点、成员快照和控制结果 | 由 RtcEngine 独占；也有独立公共入口；不构造媒体图或设备 |
| `MediaGraphBuilder` / `RtcMediaGraphBuilder` | 连接输入、编解码、同步、输出节点 | 返回未启动的 `PreparedMediaGraph`，不持有 RoomEngine |
| `PublishPipeline` / `SubscribePipeline` | 每轨 GraphEngine 的准备、启动和停止 | 引擎创建的 Pipeline 不自行解绑端点，解绑由引擎协调 |
| `MediaResources` / `RtcMediaResources` | 共享音频输出、回声参考和同步时钟 | 引擎拥有，构图器借用 |
| 应用 Session | 业务状态、订阅选择、UI 交接和应用供帧 | 不创建普通媒体图，不驱动 SDK poll |

当前没有独立的 `TrackGraphRegistry`、`RtcPipelineEngine` 或 `RtcMediaEngine` 类参与上述编排。`LiveKit::RTCMediaEngine` 是组合链接目标，不是公开引擎类。实现见 [rtc_engine.cpp](../src/engines/rtc/rtc_engine.cpp) 与 [构图器](../src/engines/rtc/media_graph_builder.cpp)。

## 入口选择

| 场景 | 入口 | CMake target |
| --- | --- | --- |
| 配置采集、编解码、预览、播放和逐轨录制 | `create_livekit_engine(AudioOutputOptions{})` | `LiveKit::RTCMediaEngine` |
| 引擎管理编码发布或自定义图 | 无参数 `create_livekit_engine()`；自定义图另含 `rtc/graph_api.hpp` | `LiveKit::RTCEngine` |
| 直接操作编码端点，不需要媒体图 | `create_livekit_room()`，含 `rtc/room_engine.hpp` | `LiveKit::RTCLiveKit` |

带音频选项的工厂返回 `Result<std::unique_ptr<RtcEngine>>`；无参数工厂直接返回 `std::unique_ptr<RtcEngine>`。也可先创建无参数引擎，再检查 `enable_media()` 的 `ErrorCode`。媒体装配应在发布/订阅前完成；当前实现会替换媒体资源，不应把重复调用 `enable_media()` 当作活动设备热切换接口。

`LIVEKIT_BUILD_RTC_MEDIA=ON` 启用可选媒体模块。没有装配媒体模块时，配置式媒体发布/订阅在构图阶段通过 Completion 返回 `invalid_state`，已建立的端点进入回滚；`create_shared_input()` 同步返回失败 Result。

## 公开接口清单

| 能力 | 当前接口 | 结果方式 |
| --- | --- | --- |
| 观察事件 | `set_observer(RtcObserver*)` | 同步 `ErrorCode`，Observer 为借用对象 |
| 入会 | `join(RoomOptions, Completion<RoomSnapshot>)` | 返回 void；结果通过 Completion |
| 退会/关闭 | `leave(Completion<void>)`、`close(Completion<void>)` | 同步屏障，close 是 leave 的别名；允许后续 join |
| 配置式媒体 | `publish_media`、`publish_video`、`publish_audio`、`subscribe_media` | Completion 返回 `MediaPublication` / `MediaSubscription` |
| 编码/空扩展图 | `publish(TrackDescriptor)`、`subscribe(TrackDescriptor)`、`subscribe(TrackId)` | 同样使用 Completion；不会自动创建原始媒体输出 |
| 轨道控制 | `unpublish`、`unsubscribe`、`set_muted`、`set_receive_demand` | `Completion<void>` |
| 成员和权限 | `update_local_participant`、`set_subscription_permissions` | `Completion<void>` |
| 状态和服务 | `snapshot()`、`services()` | `RoomSnapshot`、`shared_ptr<RoomServices>` |
| 共享输入 | `create_shared_input(TrackDescriptor, MediaInput)` | 同步 `Result<shared_ptr<SharedMediaInput>>` |
| 编码写入和反馈 | `write`、`publication`、`feedback` | `WriteStatus`、`TrackSnapshot`、`Result<SenderFeedback>` |
| 高级构图 | `graph()` 返回 `RtcGraphApi` | 图接口沿用逐次 Completion |

表中省略了部分参数；以头文件签名为准。所有 Completion 均可省略，但省略表示主动忽略结果。控制方法没有“受理码”，不能把 `join()` 或 `publish_media()` 的返回值交给 `require_result()`。普通引擎没有 `room()`、`set_room_observer()`、`set_publication_observer()` 或 `set_subscription_observer()`。

## 发布示例

以下函数展示当前可调用签名；调用方持有引擎与 Observer，`report` 必须在回调返回前完成自身工作，不得等待其他 SDK 回调。

```cpp
#include <livekit/rtc/engine.hpp>
#include <livekit/rtc/livekit.hpp>
#include <functional>
#include <utility>

void join_and_publish_microphone(
    livekit::sdk::rtc::engine::RtcEngine& rtc,
    livekit::sdk::rtc::LiveKitRoomOptions options,
    std::function<void(livekit::sdk::Result<
        livekit::sdk::rtc::engine::MediaPublication>)> report) {
  using namespace livekit::sdk;
  using namespace livekit::sdk::rtc;
  rtc.join(std::move(options), [&rtc, report](Result<RoomSnapshot> joined) {
    if (!joined) {
      report(Result<engine::MediaPublication>::failure(joined.failure_record()));
      return;
    }
    engine::MediaPublishSpec spec;
    spec.track.id = "microphone";
    spec.track.kind = MediaKind::audio;
    spec.track.source = PublicationSource::microphone;
    spec.track.format.codec = Codec::opus;
    spec.track.format.clock_rate = 48000;
    spec.track.format.channels = 1;
    spec.input.microphone = MicrophoneOptions{};
    rtc.publish_audio(std::move(spec), report);
  });
}
```

调用前使用带音频选项的工厂装配媒体模块，并检查 `set_observer(&observer)` 的结果。可运行的无窗口合成源示例见 [automatic_session](../examples/automatic_session/main.cpp)。上例配置真实麦克风，文档核对不代表已进行设备人工验收。

`publish_video` / `publish_audio` 当前只覆盖 `track.kind` 后转交 `publish_media`，不会自动补齐 codec、clock_rate 或 channels。视频通常填写 90000 时钟与宽高；Opus 填写 48000 时钟与声道。`PublicationSource` 显式表达摄像头、麦克风、屏幕等角色，默认 unknown 不会自动推断。

`MediaInput` 恰好选择一种来源：generated、文件/URL、camera/screen、应用 I420/PCM/编码源或 microphone。`preview.video` / `preview.audio` / `preview.frame` 消费发布前原始数据；`preview` 属于发布图，不是独立的入会前预览会话。编码输入直接连接发布节点，不创建编码器，也不能配置原始预览。

## 订阅与输出

在 `RtcObserver::on_room_event` 收到 `remote_track_added` 后，应用按业务选择轨道，把 `*event.descriptor` 放入 `MediaSubscribeSpec::track`，设置 `output` 并调用 `subscribe_media(spec, completion)`。沿用发现描述符可以保留 codec、远端 SID、来源身份及同步组；应用不必重新猜测远端编码格式。

- `output.video` 返回拥有像素的 `VideoSample`，`output.audio` 返回拥有 PCM 的 `AudioSample`。
- `output.frame` 提供统一的只读 `media::FrameView`，存储所有者保证各平面有效。
- `output.playout=true` 使用引擎共享音频输出；输出设备接受的格式限制与裸 PCM 回调不同。
- `output.recording` 指定逐轨 WAV/Y4M 录制目录，不代表已提供混流录制或压缩容器录制。
- `output.queue` 限制应用输出分支积压；慢回调会丢弃该分支积压，不提供无限缓冲。

`subscribe(TrackId)` 是编码/空扩展图入口，不能替代带解码和输出的 `subscribe_media()`。普通订阅默认启用音频 jitter buffer；同步默认开启，初始 presentation_delay 为 80 ms，允许范围为 0–500 ms。有 source_id 时按 source_id / stream_group 共享同步时钟；缺失发送端时钟信息时不能承诺跨轨同步。

```text
发布：输入 → 编码 → RtcPublishNode
        └→ 原始预览 / 播放 / 录制（按配置）
订阅：RtcSubscribeNode → 解码 → 同步节点（按条件）→ 回调 / 播放 / 录制
反馈：发布节点 → 输入与编码器；视频解码器 → 订阅关键帧请求
```

## 一个输入，多路发布

`create_shared_input()` 是同步 Result 接口，不接受 Completion。取得句柄后，各发布设置 `shared_input` 并清空自己的 `input`：

```cpp
// 函数内片段；rtc 已装配媒体模块，以下别名供片段使用。
namespace engine = livekit::sdk::rtc::engine;
// descriptor 与 camera 为调用方提供的有效配置，report_error/on_* 为应用函数。
auto created = rtc.create_shared_input(descriptor, camera);
if (!created) {
  report_error(created.error());
  return;
}
engine::MediaPublishSpec first;
first.track = descriptor;
first.track.id = "camera-main";
first.shared_input = *created;
auto second = first;
second.track.id = "camera-secondary";
second.track.encodings.front().max_bitrate_bps = 500000;
rtc.publish_video(first, on_first_published);
rtc.publish_video(second, on_second_published);
```

共享句柄可传给另一个房间的 RtcEngine。第一条活动发布启动源，最后一条停止采集；保留句柄可再次发布。每条发布仍有独立编码器、反馈和有界队列。共享输入支持实时原始源，不支持编码源或文件/URL，不做分支缩放；视频宽高或音频声道必须匹配，最多 128 个活动消费者。

应用拥有的 `I420VideoSource`、`PcmAudioSource`、`EncodedMediaSource` 一次只能被一个输入读者借用。unpublish、启动失败或关闭释放借用，不永久关闭源；多消费者使用共享输入。设备配置创建的会话由媒体模块管理。

## 生命周期、错误和线程

普通发布/订阅在端点 ready 后构图并启动，再交付成功 Completion；成功不表示远端已经显示首帧。构图/启动失败时，引擎停止部分图、请求解绑，再交付错误。普通撤销先停止图，再请求 RoomEngine 解绑；解绑失败不能当作资源已全部回收。

运行期后端轨道故障通过 RoomEvent 报告；媒体状态可用 `MediaTrack::snapshot()` / `check()` 查询，后者返回已记录错误的 ErrorCode，不能把未记录错误时的 ok 当作所有节点均健康的证明。当前实现没有独立失败图扫描器或自动重建状态机，不承诺每个媒体节点故障都会自动变成 Observer 事件并回收图。应用应处理失败并显式撤销或退出。编码/自定义图路径不由构图器创建 MediaTrack，成功结果的 `track` 可以为空。

control 处理房间与端点；lifecycle 处理图和媒体资源；notification 交付控制 Completion 与 Observer。RoomEngine 的内部续接也经过 notification，慢用户通知可能推迟后续构图和清理。应用媒体输出使用独立有界 dispatcher，不在 UI 线程；UI 交接由客户端负责。

回调可以提交下一次异步请求，但不能等待其他回调、同步 leave/close、替换 Observer 或销毁引擎。媒体回调不得等待生命周期操作，关闭会等待正在运行的媒体回调。Observer 须存活到解除注册返回；解除 Observer 不取消已提交的 Completion。

外部拥有者先停止应用并发请求与写入，再调用 leave/close。它们停止全部轨道、停止后端、取消待完成操作并排空通知；有效调用的可选 Completion 在返回前已执行。之后可再次 join，旧媒体句柄仅提供历史状态，重新取得当前会话的 services 并注册处理器。RoomServices 的 future 与服务执行器契约独立，不应套用控制 Completion 的契约。

## 客户端边界与当前限制

客户端负责来源和质量选择、订阅策略、业务重试、错误展示及 UI 调度；SDK 负责端点、媒体图、编解码节点、队列与资源生命周期。普通客户端无需操作 GraphNode 或维护 SDK 的执行队列。

当前仍需配置 TrackDescriptor 的媒体类型、编码格式、时钟和声道等字段；MediaInput 仍是多个可选字段的组合；MediaTrackSnapshot 仍包含节点/边诊断。类型化来源、自动补齐普通媒体参数、按远端句柄订阅、独立预览和分层统计属于后续接口改进方向，当前没有这些统一接口。

自动 H265 → H264 重发布策略已移除；`h264_fallback_input` 等保留字段不能作为该策略已实现的证据。当前每条视频发布只有一个 encoding，未实现 simulcast/SVC。后台恢复能力以具体后端为准，不保证无缝迁移或应用媒体图自动重建。

本次文档更新验证：两段媒体接入 C++ 示例经 MSVC C++20 `/Zs` 语法检查通过（共享输入片段补入函数参数及错误处理声明）；修改文档的本地链接与 Markdown 代码块检查通过。未运行完整工程构建、SFU 集成或实际设备人工验收；历史自动化结果以各日期验证记录为准。
