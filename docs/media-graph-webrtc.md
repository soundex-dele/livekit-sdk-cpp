# WebRTC 媒体图节点

实现日期：2026-10-26；采集、视频编码、解码、渲染和录制已拆成独立节点，使用 GraphEngine 创建实际事件图，发送侧仍由 media_graph 主动驱动真实编码。设计背景见 [接入方案](../../docs/webrtc-media-graph-design.md)。本实现保留 Room/Participant/TrackPublication 与 Player API，默认 RTC runtime 使用图节点工厂。

## 构建边界

`cmake/MediaGraph.cmake` 创建公共 `LiveKit::MediaCore`、`LiveKit::MediaGraph`。GraphFlow 是私有实现依赖；公共图目标不链接 FFmpeg 或 WebRTC。Player 的 FFmpeg 构图归 `LiveKit::Player`，RTC 编解码适配位于 `LiveKit::MediaNodesWebRTC`。原 `LiveKit::SDK` 链接 RTC 节点库，Player-only 构建继续不需要 RTC。

## 实际链路

```text
视频发送：
VideoSourceNode / CameraCaptureNode / ScreenCaptureNode
  ├─ VideoRenderNode → 已有 latest-frame 输出（可选本地预览）
  └─ VideoEncodeInputNode（图入口）
      → VideoEncoderNode（有界编码队列 + 真实编码）
      → VideoTrackInputNode / 自定义 native VideoFrame 携带编码包
      → WebRTC VideoStreamEncoder → PassthroughEncoder / RtcPublishNode
      → OnEncodedImage → WebRTC RTP / pacer

视频接收：
WebRTC RTP / 组帧 / 抖动缓冲 → DecoderAdapter
  → RtcDecodeInputNode → VideoDecoderNode → RtcDecodeOutputNode
  → 解码完成回调 → WebRTC 正常播放时序 → TrackSink / RtcSubscribeNode
  ├─ VideoRenderNode → SDL / I420 应用帧回调
  └─ RecordingNode → 有界队列 → Y4M 文件（可选）

设备音频：
原生 ADM → GraphAudioDevice → AudioCaptureNode → 原生 AudioTransport / APM
  → AudioEncoderAdapter → AudioEncoderNode → WebRTC RTP

WebRTC NetEq → AudioDecoderAdapter → AudioDecoderNode → 返回 NetEq / 混音
  → GraphAudioDevice / AudioOutputNode → 原生 ADM
```

真实视频编码由采集输入直接驱动，在图线程完成，不等待 WebRTC 调用 `VideoEncoder::Encode()`。标准 EncoderFactory 注册的是 **透传适配器**：它的 Encode 只解包并回交已编码帧。WebRTC 的 VideoStreamEncoder/FrameCadenceAdapter 仍调度这些触发帧，并处理协商、RTP/RTCP、加密、重传和 pacer。因此这是“不让 WebRTC 驱动真实编码”，不是“完全绕过 WebRTC 发送调度”。

接入参考 CustomVideoTrackSource + passthrough 的思路，但不分配黑色 I420 占位图，也不使用全局编码器注册表或分离的编码包 FIFO。每个自定义 native buffer 同时携带编码包、元数据、配置代次和弱引用源标识；触发帧被丢弃时不会使后续包错配，并请求图编码器产生新的关键帧恢复参考链。

启动时，源先发无编码负载的元数据探测帧；透传适配器从正常 InitEncode 获得协商格式、尺寸和参数，并通过该帧绑定具体源。下一帧起图创建真实编码器并主动编码。探测帧不作为画面发送。SetRates、RTT/loss 和关键帧请求通过每轨状态回传图，codec 操作全部串行发生在图线程。

图最多保留 1 个待编码原始视频帧；满队列淘汰最旧原始帧，不淘汰控制消息。已经编码的触发帧如被 WebRTC 丢弃，通过关键帧恢复；不任意删除参考帧后继续发送依赖它的 delta 帧。

音频保留上一版路径，由 WebRTC 驱动 AudioEncoderNode/AudioDecoderNode，保留 ADM、APM、NetEq、DTX/FEC/PLC。自定义 VideoTrackSource 不能用于音频，本次没有声称音频也改成外部编码包发布。合成音调/文件音频继续通过独立 AudioSourceNode 输入轨道。

`VideoRenderNode` 调用现有输出目标的有界 latest-frame 入口，SDL 渲染与应用回调仍在各自既有会话线程执行。慢应用回调仍可能拖慢会话控制，但不会在 WebRTC 接收回调里执行，也不会每帧积压一个应用回调。

录制分支不再在 track sink 回调中进行磁盘 I/O。每轨录制队列最多 32 项、按帧大小估计最多 64 MiB；超限拒绝新数据并报告错误，不阻塞 RTC。当前沿用 Room 的录制错误策略：会话检查到错误后结束会话。停止先解除 sink，再排空录制队列、完成 WAV 文件头。

## GraphEngine 构图与连接

`RtcPipeline` 持有 `graphflow::engine::GraphEngine`，通过 `loadFromNodes(nodes, connections)` 建图，再由引擎统一执行 init/start/stop/release。使用默认 EventBus 传输，不使用只描述拓扑的 External 边；媒体确实通过节点的 `publish<Event>()` / `subscribe<Event>()` 流动。

- 每个发布轨道拥有一张图：CaptureNode → VideoEncodeInputNode → VideoEncoderNode → VideoTrackInputNode；CaptureNode 可分流到 VideoRenderNode。MediaPump 只负责组装与控制，采集线程、设备和文件输入实现在独立的 `media_nodes/capture_source.*`。
- 每个 DecoderFactory 实例拥有一张图：RtcDecodeInputNode → VideoDecoderNode → RtcDecodeOutputNode。输出通过标准回调返回 WebRTC，保留其播放同步和缓冲。
- 每个接收轨道拥有一张输出图：RtcSubscribeNode → VideoRenderNode / RecordingNode。TrackSink 只注入图入口，不再直接调用渲染或录制节点。
- 文件/合成音频输入使用 AudioSourceNode → AudioTrackInputNode；麦克风、音频 codec 和 NetEq 保留原有设备/工厂适配。

构图示意（节点实例均为 Created 状态）：

```cpp
RtcPipeline pipeline(registry);
pipeline.build({capture, input, encoder, output, preview}, {
    RtcPipeline::connect<RawVideoFrame>(capture, input),
    RtcPipeline::connect<RawVideoFrame>(input, encoder),
    RtcPipeline::connect<EncodedVideoFrame>(encoder, output),
    RtcPipeline::connect<RawVideoFrame>(capture, preview),
});
pipeline.start();
// 所有下游启动完成后，CaptureNode::activate() 才启动采集线程。
```

图边同步派发，只负责传递数据；跨线程和背压由处理节点负责。VideoEncoderNode 使用独立 EncodeExecutor，最多保留 1 个待编码原始帧；RecordingNode 使用自己的有界磁盘队列；渲染使用已有 latest-frame 输出。没有为每帧向 GraphEngine 的通用线程池投递无限异步任务。RawVideoFrame/EncodedVideoFrame 保留引用计数 buffer；PCM 和解码请求/返回值的借用只在同步调用期间有效。

节点在每个进程内有唯一名称，多轨不会因类型相同而连接到一起。拓扑加载后不在编解码回调中修改；协商变化只重建 VideoEncoderNode 内部 codec。停止按节点顺序先 join 采集，再关闭编码准入并等待 codec 回调屏障，最后停止输出；release 后关闭图边。接收侧先解除 TrackSink，再停止输出图。重启复用订阅，录制队列重新开启并写入新的文件分段。

## Factory 与生命周期

`src/backends/webrtc/codec_factories.*` 实现四类 Graph factories。视频 EncoderFactory 的 `Create(env, format)` 创建透传适配器，协商参数与源绑定后由已建图的 VideoEncoderNode 创建真实 codec；DecoderFactory 创建独立的解码子图。`room.cpp` 在 `EnableMediaWithDefaults(deps)` **之前**注入 factories；内置 helper 只补空缺配置。

视频真实 codec 分别由 VideoEncoderNode 和 VideoDecoderNode 持有，两者继承 MediaNode/GraphNode；音频保留 `CodecNode<Backend>`。GraphVideoSender 负责提供编码子图的节点与边，视频透传适配器持有每轨协商/反馈状态。默认 delegate 使用 WebRTC builtin factories；私有 `CodecBackends` 注入结构允许替换后端及测试工厂，不是对外稳定的插件 ABI。

节点保持编码参数、单空间层的时间层码率、关键帧请求和 RTT/loss 反馈；音频保留 FEC 等原生控制。编码包、codec-specific 元数据及 native frame 直接按 WebRTC 引用契约传递，不经过有损的公共字节数组转换。视频 callback 回交 WebRTC，渲染不绕过接收输出时序。

音频编码在 WebRTC 原有序列内同步执行；解码包装真实 `ParsePayload → EncodedAudioFrame::Decode` 路径，保留 duration、DTX、PLC、FEC 与网络自适应行为。解析出的音频帧持有 decoder 节点租约，帧销毁前 backend 不会提前释放。

`RtcGraph` 保留为会话内弱引用诊断注册表，提供节点指标、唯一名称和连接关系；`RtcPipeline` 中的 GraphEngine 实际拥有视频节点并管理拓扑与生命周期。节点完成建图后才登记，registry 不驱动这些节点的初始化。音频及透传边界适配仍支持原有 add 接口。快照读取原子计数与受锁保护的 codec 格式，关闭注册表后拒绝新图准入。

Factory 没有 track ID，实例 ID 不能当作 track ID/SSRC；不根据创建顺序绑定轨道。默认 builtin encoder 内部可以使用 simulcast adapter，节点同时保留其 delegate factory 的寿命，避免 factory 先销毁后的悬空引用。

停止先关闭图准入、停止采集，再暂停图编码器并等待真实 codec 的 Release 回调屏障，随后关闭 PC。透传适配器的 callback 只在 WebRTC 的 Encode 调用内使用；它不持有源的裸指针。旧 native buffer 只保留源的弱引用和状态租约，关闭后不能重新启动编码。支持采集暂停/恢复和 codec 重建，图使用弱引用，不无限保留历史 codec。

## 采集与预览

`MediaSpec` 末尾新增 `capture`、`device_index`、`local_preview`，保留已有聚合初始化用法。

```cpp
#include <livekit/livekit.hpp>

auto devices = livekit::sdk::video_capture_devices();
auto camera = livekit::sdk::probe_media("camera://0").front();
camera.local_preview = true;

livekit::sdk::RoomOptions options;
options.url = server_url;
options.token = room_token;
options.media = {camera};
options.stats_interval_ms = 500;
options.video_frame_handler = [](const livekit::sdk::VideoFrame& frame) {
  // 本地预览 track_id 前缀为 local-；远端仍使用原轨道 ID。
  // 应用线程内处理；需要保留时复制帧，耗时任务交给应用自己的队列。
};
livekit::sdk::Room room;
room.run(options);
```

摄像头由当前 WebRTC video capture 后端采集、枚举并选择最接近的设备模式。源保留 `AdaptedVideoTrackSource` 连接接口，广播的是 native 编码包封装；真实编码前的尺寸/帧率调整由图执行，编码后不再裁剪、转换像素。预览分支位于编码前。

屏幕输入为 `probe_media("screen://primary")`，或显式设置 `MediaSpec.capture = CaptureKind::screen`。当前 Windows GDI 后端捕获主屏并输出 I420；screencast 与信令 SCREEN_SHARE 来源分别设置。摄像头/屏幕输入不启动 FFmpeg。

CLI 可直接使用已有 `--publish` 参数：

```powershell
lk room publish example --identity camera --publish camera://0 --duration 20
lk room publish example --identity screen --publish screen://primary --stats-interval-ms 500
```

麦克风仍由 `RoomOptions.microphone` 启用，播放由 `audio_playout` 启用；原生 ADM 与图输入输出节点共享同一个设备实例。没有音频设备时保留原有 headless 10 ms 输出时钟，驱动 NetEq 和订阅 PCM。

## 观测与测试

启用 `stats_interval_ms` 后，除已有 `rtc-stats` 外增加 `media-graph-stats` 事件，包含每节点的 `id/name/kind/format/inputs/outputs/errors/dropped/generation`，以及 `edges[{from,to,dataType}]` 连接表。编解码次数按实际提交/回调统计，不把采集帧、编码层、解码和呈现混为同一口径。WebRTC 可见的视频透传和解码适配器 implementation name 带 `media_graph/` 前缀。

```powershell
cmake --build out/windows-x64 --config Release --parallel 4
ctest --test-dir out/windows-x64 -C Release --output-on-failure
python tests/media_graph_integration.py `
  --lk out/windows-x64/Release/lk.exe `
  --server D:/path/to/livekit-server.exe `
  --sdk-client out/windows-x64/Release/livekit_sdk_integration.exe
```

`media_graph` 原生测试覆盖 GraphEngine 实际边、分流、无效端点拒绝、停止后拒收、重启不重复订阅、采集和录制重启，以及真实 VP8 编解码、关键帧/时间戳/codec metadata、重复初始化、同格式多实例、factory 提前释放、Opus 比特流与 builtin 对照、解析音频帧的资源租约、ADM 输入输出/时间戳反馈、队列过载/关闭/代次、本地预览和 WAV/Y4M 排空。

透传回归还验证真实编码在透传 Encode 被调用前完成、源/编码器逐轨绑定、丢弃触发帧后的关键帧恢复、重复 native buffer 去重、静音黑帧不会透传源内容、源暂停/恢复及关闭后不保留图节点。

集成脚本启动独立本地 C++ SFU，验证真实音视频、数据通道、四类 codec 节点和录制输出；可同时运行既有动态发布订阅/Room 复用回归。所有进程由脚本创建并清理，输出保存在 `out/media-graph-integration/`。

本次 GraphEngine 版本验证（2026-10-26，Windows x64 Release）：

- SDK/CLI/Demo 构建通过：`out/graph-engine-build-final.log`。
- RTC CTest **10/10 通过**：`out/graph-engine-ctest.log`。
- Player-only（FFmpeg 7.1 缓存配置）构建通过，CTest **7/7 通过**，包含无窗口输出测试：`out/graph-engine-player-build-final.log`、`out/graph-engine-player-ctest.log`。
- SDK → C++ SFU → SDK 通过，包含真实 VP8/Opus、录制、数据通道、动态发布订阅和 Room 复用；集成测试同时验证了实际 GraphEngine 连接表。证据：[result.json](../out/media-graph-integration/20261026-155338/result.json)，其中 `graphEngineTopology: true`，保留服务端和 CLI 的 SHA-256。
- WebRTC 源码未修改；SFU 使用既有二进制。

GraphEngine 改造前标准接口透传版本的历史验证（2026-10-26，Windows x64 Release）：

- SDK/CLI/Demo 与测试目标构建通过，记录见 `out/graph-passthrough-build-final.log`。
- CTest **10/10 通过**，包括新增透传/图编码回归，记录见 `out/graph-passthrough-ctest.log`。
- SDK → C++ SFU → SDK 通过：真实 VP8/Opus、数据通道、WAV/Y4M 录制、动态发布订阅和 Room 复用；[最终 result.json](../out/media-graph-integration/20261026-144053/result.json) 包含二进制 SHA-256。
- WebRTC 原生源码无本轮差异；其原先存在的 README/设计文档改动保留。SFU 使用既有二进制，本次未重建。物理设备、硬件 codec、跨平台与弱网质量不在此次自动化验收范围内。

2026-10-26 Windows x64 上一版（WebRTC 调度编码）的历史验证结果，不能作为本次直接编码变更的验证：

| 验证 | 结果 |
| --- | --- |
| RTC Release 全量构建 | 通过，包含 SDK、CLI、Demo 与测试目标 |
| RTC CTest | 8/8 通过，包含新增 media_graph 与既有帧输出/SDK/SDL 回归 |
| Player-only Release 构建及 CTest | 通过，5/5 测试通过 |
| RTC + Player 同时开启配置 | CMake configure/generate 通过；未重建整套组合配置 |
| SDK → C++ SFU → SDK | 四类 codec 节点实际处理媒体；数据通道、有效 WAV/Y4M、动态发布订阅及 Room 复用全部通过 |

最终集成证据：[result.json](../out/media-graph-integration/20261026-115853/result.json)。使用本机既有 `D:/workspace/livekit-cpp/out/windows-x64/Release/livekit-server.exe`，本次未重新构建 SFU；报告包含服务端与新 CLI 二进制 SHA-256。未将这次合成源回归等同于历史文件四轨、跨系统或弱网回归全部修复。

## 标准接口与维护边界

本实现不修改 WebRTC 源码或增加其工厂虚函数。视频只使用标准 `VideoTrackSourceInterface`、`VideoFrameBuffer::kNative`、`VideoEncoderFactory`、`VideoEncoder` 和 `EncodedImageCallback`。本轮曾尝试的原生发送流扩展已撤回。

- 透传的 GetEncoderInfo 声明支持 native handle、关闭像素质量缩放并声明可信码率控制；不会伪报硬件编码器。
- 当前支持每轨单编码流、单空间层；多空间层能力不对外宣告，多路 simulcast 配置明确拒绝，不静默错发。仍可同时发布多个独立轨道。
- WebRTC 可能在 Encode 之前丢弃触发帧；已增加关键帧恢复，但不能宣称完全绕过 WebRTC 的发送调度或无损透传所有编码帧。
- WebRTC 静音时生成的黑色像素帧被透传适配器丢弃，不在适配器内重新编码；静音期间停止转发新视频，恢复时发送关键帧。屏幕 cadence 重复同一 native buffer 时去重，不把相同压缩参考帧作为新帧重放。
- 当前 WebRTC 的 SetEncoderRates 不会把零码率转交给编码器；图会处理收到的码率反馈，但不能承诺仅靠 SetRates 在网络暂停时立即停止真实编码。WebRTC 仍控制实际发送准入。
- H265 需要后端及协商双方支持；参考代码中的 H265 透传不意味着此 SDK 的默认 builtin 后端具备 H265 编码能力。

## 当前限制

- 本次首发路径是 Windows x64、CPU/I420、VP8/Opus。Factory 转发其他 builtin 能力，不等于已验收其他 codec、平台或硬件后端。
- 屏幕后端当前只支持主屏，无窗口选择、系统音频、鼠标合成或 GPU 零拷贝；摄像头通过设备序号选择，无热插拔恢复。
- 未实现跨 Room/Player 的统一设备租约、混音/AEC 参考协调，不承诺并行打开同一物理设备。
- 视频使用内部 GraphEngine 实例和显式连接表，尚未开放任意 JSON 媒体拓扑、公共高级图 API、逐轨自定义 codec backend、无缝换图或外部预编码包发布。
- 播放器的公共 RGBA/float32 帧模型保留，RTC 内部继续使用引用计数 native frames；通用多平面/GPU 公共 Buffer API 尚未开放。
- 物理摄像头、麦克风、扬声器的设备质量、真实 AEC/长期 A-V 同步与屏幕端到端质量需要交互式设备验收。自动化测试不代替这些验收。
