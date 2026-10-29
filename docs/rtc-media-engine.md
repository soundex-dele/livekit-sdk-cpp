# 统一 RTC 引擎与配置式媒体 API

普通媒体会话使用 `livekit/rtc/engine.hpp`，链接 `LiveKit::RTCMediaEngine`。`RtcEngine` 是统一公开入口，拥有一个 `RoomEngine`、内部 `RtcPipelineEngine` 和可选的内部 `RtcMediaEngine`。应用 Session 负责业务状态、配置和 UI 数据交接；媒体模块创建采集、编解码、输出节点，流水线模块管理每轨 GraphEngine，房间模块管理成员、信令、发布订阅端点及数据服务。内部模块借用顶层拥有的对象，不反向持有 RtcEngine。

RtcEngine 使用 PImpl：公开类仅持有 `std::unique_ptr<Impl>`，主管线程、关闭状态、房间、流水线、可选媒体模块和 RoomView 均封装在私有实现中。析构在实现文件定义，继续保证先停止图与回调，再释放媒体和房间；媒体模块保持可选链接。

`RoomSession` 和公开的 `RtcMediaEngine` 已移除，无兼容接口。配置和媒体句柄位于 `media_config.hpp`，由 `engine.hpp` 包含。高级图接口直接通过 RtcEngine 的 `publish_graph` / `subscribe_graph` 使用。

仅收发已有编码帧时，继续使用 [编码媒体 API](rtc-encoded-api.md) 的 RoomEngine 或 RtcEngine，无须链接媒体引擎。`LIVEKIT_BUILD_RTC_MEDIA=ON` 启用可选媒体引擎和媒体节点库，不改变纯传输层依赖。

## 发布与订阅

```cpp
using namespace livekit::sdk::rtc;
auto engine = engine::create_livekit_engine(AudioOutputOptions{});
RoomOptions room;
room.url = url;
room.token = token;
engine->join(room).get();

engine::MediaPublishSpec config;
config.track.id = "camera";
config.track.format.codec = Codec::vp8;
config.track.format.width = 1280;
config.track.format.height = 720;
config.input.capture = livekit::sdk::CaptureKind::camera;
config.input.fps = 30;
config.preview.video = [](const engine::VideoSample& frame) {
  // 把 frame.pixels 交给应用的有界最新帧队列；UI 在线程所属位置呈现。
};
auto publication = engine->publish_video(config);
// 主管线程持续 engine->poll()，随后检查 publication.ready。
// publication.track->snapshot() 提供状态、计数和图拓扑。
```

`publish_media` 自动按轨道类型选择编码器，`publish_video` / `publish_audio` 额外检查类型。`MediaInput` 恰好选择一种输入：generated、文件/URL、camera/screen、应用 I420/PCM/编码源，或 microphone 配置。麦克风配置由引擎创建设备会话，构造本身不打开设备。

`subscribe_media(MediaSubscribeSpec)` 接收远端 SID 和媒体格式；`output.video` 返回拥有像素的 `VideoSample`，`output.audio` 返回拥有 PCM 的 `AudioSample`。`output.playout=true` 启用共享音频输出设备，`output.recording` 启用逐轨 WAV/Y4M 录制。`preview` 使用同一输出配置，消费发布前原始数据。非 48 kHz 应用 PCM 可编码并通过回调预览；直接设备播放要求 48 kHz PCM16。

引擎内部的主要连接为：

```text
发布：输入 → 视频/音频编码 → RtcPublishNode
        └→ 预览回调 / 音频播放 / 原始录制（按配置）
订阅：RtcSubscribeNode → 视频/音频解码 → 回调 / 播放 / 录制
反馈：发布节点 → 输入与编码器；视频解码器 → 订阅关键帧请求
```

编码输入直接连接发布节点，不创建编解码器，也不能配置原始预览。音频、视频拥有各自的 TrackId 和图；一条视频发布只代表一个编码层。

## 一个采集源，多路发布

显式创建 `SharedMediaInput`，引擎通过独立源图把原始帧分发到各发布的有界输入队列：

```cpp
engine::MediaInput camera;
camera.capture = livekit::sdk::CaptureKind::camera;
camera.fps = 30;
auto shared = engine->create_shared_input(config.track, camera);

auto first = config;
first.track.id = "camera-main";
first.input = {};
first.shared_input = shared;
auto second = first;
second.track.id = "camera-secondary";
second.track.encodings.front().max_bitrate_bps = 500000;
auto a = engine->publish_video(first);
auto b = engine->publish_video(second);
```

共享句柄也可传给另一个房间的 RtcEngine。第一条活动发布启动采集，最后一条关闭时停止并释放读者；保留句柄可再次发布。每条发布有独立编码器、队列、反馈和关闭屏障；停止一路不关闭其他路或房间。共享采集仅支持实时原始源，不支持编码源、文件/URL，不做逐分支缩放：视频尺寸和音频声道数必须匹配。当前最多 128 个活动消费者，各分支满载时按应用源有界队列策略拒绝数据，不等待容量；共享图本身仍会产生逐分支复制开销。

外部 `I420VideoSource`、`PcmAudioSource` 和 `EncodedMediaSource` 由应用拥有，单个源只能有一个输入读者。普通发布借用该读者；取消、启动失败、unpublish 或 close 释放借用，不调用源的永久 close。需要多个消费者时通过共享输入扇出。设备配置产生的设备会话由引擎管理。

## 线程与生命周期

- 创建、控制、poll、关闭和析构在同一个主管线程执行。媒体操作的 ready future 由 poll 完成；不要在停止 poll 后阻塞等待发布/订阅 ready。
- 像素/PCM 回调在输入或 codec worker 上运行，必须有界、非阻塞，不可重入引擎生命周期；转交 UI 时使用拥有数据的样本和有界队列。
- MediaTrack 句柄可供快照读取。保留句柄不会让已关闭图继续工作；异步创建失败记录到快照并可通过 throw_if_failed 查询。
- 关闭先停止生产者、等待编解码及输出，再拆除图。完成后应用可释放回调目标；录制会完成文件头。关闭房间会关闭全部轨道。
- RtcEngine 的 `publish_graph` / `subscribe_graph` 是自定义图的高级入口；普通 Session 不需要传入 GraphNode 或 graph factory。

会议、投屏、远控、Studio、CLI 和音视频示例均使用配置式入口；`room_client` 保留纯编码/高级图示例。当前媒体引擎没有补充 AEC/APM、NetEq/PLC、跨轨音画同步或 simulcast/SVC。

## 房间接口与生命周期

```cpp
auto snapshot = engine->room().snapshot();
auto services = engine->room().services();
auto update = engine->room().update_local_participant(participant_update);
while (auto event = engine->next_event(std::chrono::milliseconds{0})) {
  // 房间事件只有一个消费者；内部流水线观察端点状态，不消费这条队列。
}
engine->leave().get();  // 先停止媒体图并完成关闭屏障，再退出房间。
engine->join(other_room).get();  // 新适配器、新 epoch，可复用同一个引擎。
engine->close().get();  // 永久关闭；重复 close 安全，之后不能 join。
```

`room()` 返回借用的 `const RoomView&`，有效期到引擎销毁；只开放快照、成员更新、订阅权限和房间服务，不开放 join、leave、端点发布和销毁。轨道生命周期操作统一通过 RtcEngine。RoomEngine 仍可单独用于不需要图的纯编码传输。

RoomEngine 保留连接状态机、成员快照、期望轨道注册表和实际端点绑定。底层 RoomTransport 恢复网络连接，RoomEngine 恢复期望发布订阅；不得在两个层级重复重放。重入房间创建全新适配器与房间服务，旧服务句柄保持关闭，旧端点不能影响新绑定。用户需要重新取得 `services()` 并注册新会话的 RPC/流处理器。

leave 会取消尚未完成的媒体/图操作并停止当前房间全部图；共享采集源的其他房间消费者仍可运行。保留的媒体句柄只提供历史状态。媒体操作成功表示端点确认且图已经启动；图构建或启动失败会撤销已创建端点。

只有 `create_livekit_engine(AudioOutputOptions{})` 或首次 join 前调用 `enable_media()` 才装配媒体模块。无参数工厂创建编码模式引擎，链接 `LiveKit::RTCEngine` 即可；未启用媒体时调用配置式媒体接口返回 `unsupported` 异常。两种模式都使用同一个 RtcEngine 类型，纯传输构建不需要媒体设备或编解码实现。
