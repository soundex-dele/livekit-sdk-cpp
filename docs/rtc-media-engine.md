# 配置式 RTC 媒体引擎

普通媒体会话使用 `livekit/rtc/media_engine.hpp`，链接 `LiveKit::RTCMediaEngine`。Session 负责业务状态、发布/订阅配置和 UI 数据交接；`RtcMediaEngine` 创建采集、编解码、输出节点并组织 GraphEngine，`RtcEngine` 管理每轨传输图，`RoomBackend` 管理共享房间及信令、编码帧传输。

仅收发已有编码帧时，继续使用 [编码媒体 API](rtc-encoded-api.md) 的 RoomBackend 或 RtcEngine，无须链接媒体引擎。`LIVEKIT_BUILD_RTC_MEDIA=ON` 启用可选媒体引擎和媒体节点库，不改变纯传输层依赖。

## 发布与订阅

```cpp
using namespace livekit::sdk::rtc;
auto engine = engine::create_livekit_media_engine();
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

共享句柄也可传给另一个房间的 RtcMediaEngine。第一条活动发布启动采集，最后一条关闭时停止并释放读者；保留句柄可再次发布。每条发布有独立编码器、队列、反馈和关闭屏障；停止一路不关闭其他路或房间。共享采集仅支持实时原始源，不支持编码源、文件/URL，不做逐分支缩放：视频尺寸和音频声道数必须匹配。当前最多 128 个活动消费者，各分支满载时按应用源有界队列策略拒绝数据，不等待容量；共享图本身仍会产生逐分支复制开销。

外部 `I420VideoSource`、`PcmAudioSource` 和 `EncodedMediaSource` 由应用拥有，单个源只能有一个输入读者。普通发布借用该读者；取消、启动失败、unpublish 或 close 释放借用，不调用源的永久 close。需要多个消费者时通过共享输入扇出。设备配置产生的设备会话由引擎管理。

## 线程与生命周期

- 创建、控制、poll、关闭和析构在同一个主管线程执行。媒体操作的 ready future 由 poll 完成；不要在停止 poll 后阻塞等待发布/订阅 ready。
- 像素/PCM 回调在输入或 codec worker 上运行，必须有界、非阻塞，不可重入引擎生命周期；转交 UI 时使用拥有数据的样本和有界队列。
- MediaTrack 句柄可供快照读取。保留句柄不会让已关闭图继续工作；异步创建失败记录到快照并可通过 throw_if_failed 查询。
- 关闭先停止生产者、等待编解码及输出，再拆除图。完成后应用可释放回调目标；录制会完成文件头。关闭房间会关闭全部轨道。
- `encoded_engine()` 是自定义图的高级入口；普通 Session 不需要传入 GraphNode 或 graph factory。

会议、投屏、远控、Studio、CLI 和音视频示例均使用配置式入口；`room_client` 保留纯编码/高级图示例。当前媒体引擎没有补充 AEC/APM、NetEq/PLC、跨轨音画同步或 simulcast/SVC。
