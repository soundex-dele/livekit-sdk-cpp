# 本地文件播放器

首版播放器按统一 SDK 架构落在本仓库中，入口为 `livekit::sdk::Player`、`LiveKit::Player`，与 RTC backend 独立构建。FFmpeg 通过动态库解封装、软件解码和格式转换；SDK 不启动 ffmpeg 子进程播放。测试素材生成使用 ffmpeg 命令行。

## 引擎构建

桌面界面、应用测试和 Windows 构建脚本已迁至 [livekit-app 播放器](../../livekit-app/apps/player/desktop/README.md)。SDK 仅提供 `LiveKit::Player`，不依赖 Qt、ImGui 或 SDL。桌面应用已迁移为 Qt6 Widgets，使用 QPainter / QAudioSink 消费 SDK 帧；公共播放器 API 保持不变。

```powershell
cmake -S . -B out/player-engine -G 'Visual Studio 17 2022' -A x64 `
  -DLIVEKIT_BUILD_RTC_NATIVE=OFF -DLIVEKIT_BUILD_RTC_GRAPH_NODES=OFF -DLIVEKIT_BUILD_RTC_MEDIA=OFF `
  -DLIVEKIT_BUILD_CLI=OFF -DLIVEKIT_BUILD_DEMO=OFF -DLIVEKIT_BUILD_EXAMPLES=OFF -DLIVEKIT_BUILD_SERVER_SDK=OFF -DLIVEKIT_BUILD_PLAYER=ON `
  -DLIVEKIT_FFMPEG_ROOT='D:/ffmpeg-dev'
cmake --build out/player-engine --config Release --parallel 4
ctest --test-dir out/player-engine -C Release --output-on-failure
```

需要匹配的 FFmpeg 头文件、库和运行时；可用 `LIVEKIT_FFMPEG_INCLUDE` 指定独立头文件目录。`LIVEKIT_PLAYER_BUILD_TESTS=OFF` 关闭引擎测试后不需要 ffmpeg 可执行文件。`LIVEKIT_BUILD_PLAYER` 默认关闭；目前仅提供源码集成，尚未提供安装包/C ABI。

## 分层与执行

| 目录 / target | 职责 |
| --- | --- |
| `include/livekit/media_core` / `LiveKit::MediaCore` | SDK 自有 RGBA/PCM 帧、共享只读 Buffer、微秒时间戳、generation、steady clock |
| `src/media_nodes` / `livekit_media_nodes_ffmpeg` | 独立的文件源、解封装、音视频解码、视频转换、音频重采样节点 |
| `src/engines/player/playback_graph.*` / `LiveKit::Player` | GraphEngine::loadFromNodes 装配、引擎生命周期和播放控制 |
| `src/media_graph/media_flow.hpp` | 通用图执行器、类型化发布/订阅、接收确认、背压、epoch 和错误回传 |
| `src/media_nodes/render/playback_output.hpp` / `LiveKit::Player` | 图内音视频输出节点，各自拥有有界 MediaChannel，支持跨线程拉取 |
| `src/engines/player` / `LiveKit::Player` | 场景状态机、命令队列、播放时钟和按时取帧 |
| `../livekit-app/apps/player/desktop/main.cpp` | Qt6 应用入口、命令行、隔离设置和无窗口运行 |
| `../livekit-app/apps/player/desktop/controller.*`、`window.*` | SDK 公共 API 消费者；异步会话控制、音视频输出、中文播放器界面 |

Player 独占内部图。`PlaybackGraph` 包含 **8 个实际 GraphNode 和 7 条带类型的媒体连接**：

```text
FileSourceNode → DemuxerNode
                   ├→ VideoDecoderNode → VideoConverterNode → VideoFrameOutputNode
                   └→ AudioDecoderNode → AudioResamplerNode → AudioFrameOutputNode
```

图通过 `graphflow::engine::GraphEngine::loadFromNodes(nodes, connections)` 一次性加载 8 个节点和 7 条 `NodeConnection`。播放器不再手工注册 GraphBuilder 节点、创建 GraphPath 或持有独立控制总线。GraphEngine 负责绑定控制总线、创建带类型的 EventBus 连接、持有节点和路径，并统一执行 `init/start/stop/release`。Demuxer 的 `video` / `audio` 路由分别连接两个解码器，媒体事件仍沿图边实际传递。

`loadFromNodes` 按传入节点顺序驱动生命周期，因此播放器传入下游到上游的节点顺序，让订阅者先初始化、启动，也让文件源最后释放。`engine.init()` 完成后，再按媒体拓扑顺序调用 `prepare()`，此时源文件已打开、所有订阅已建立，Demuxer 才执行探测并发布流参数。准备阶段发生错误时调用 `engine.release()` 回滚整图。

| 节点 ID | 实现文件 | 所有权与职责 |
| --- | --- | --- |
| `file.source` | `src/media_nodes/capture/file_source.cpp` | 文件句柄；按 IO 请求读取并发布 SeekableBytes，处理 Seek、取消/超时 |
| `file.demuxer` | `src/media_nodes/demux/ffmpeg_demuxer.cpp` | 自定义 AVIO、容器上下文；订阅字节，发布流参数、VideoPacket / AudioPacket 和 EOS |
| `video.decoder` / `audio.decoder` | `src/media_nodes/codecs/ffmpeg_decoder.cpp` | 订阅流参数/编码包，持有各自解码器，发布 DecodedVideo / DecodedAudio 和 EOS |
| `video.rgba` | `src/media_nodes/process/ffmpeg_video_converter.cpp` | swscale 上下文；RGBA 转换、过滤 Seek 目标之前的帧 |
| `audio.resampler` | `src/media_nodes/process/ffmpeg_audio_resampler.cpp` | swresample 上下文；48 kHz 双声道 PCM、Seek 裁剪、重采样尾部排空 |
| `video.output` / `audio.output` | `src/media_nodes/render/playback_output.hpp` | 有界帧队列、generation、按时间拉取、线程间同步 |

`MediaExecutor` 根据实际连接计算拓扑顺序，每次从下游向上游执行一轮节点 `pump()`。执行器不识别 FFmpeg 节点类型，`PlaybackGraph::step()` 不再调用 read/send/receive/convert。节点通过 `subscribe_media<T>()` 接收事件，在自己的有界输入槽中保存数据，处理后通过 `publish_media<T>()` 沿图边发布。

发布/订阅在 Player 工作线程内同步派发，接收回调负责接收数据和返回确认，常规解码与转换在后续调度中完成。节点满载时返回拒收，上游保留自己的待发布事件，下轮重试；每条输出端口只允许一个订阅者，避免重试导致分支重复投递。当前不调用 EventBus 的无界异步入队接口，也不为每个节点创建线程。整轮无进展时执行器返回 `backpressured`，Player 让出执行处理命令或等待输出被消费。

文件字节读取适配 FFmpeg 的同步 AVIO 契约：Demuxer 在 AVIO 回调里通过控制总线向源节点请求读取，FileSource 发布最多 32 KiB 的 `SeekableBytes`，Demuxer 的订阅回调将数据填入 AVIO 缓冲。Seek/文件大小查询走同一 IO 控制接口。源节点和解封装器之间没有直接的方法调用。所有节点完成初始化和订阅后，Demuxer 发布含自有编解码参数的准备事件，解码器据此打开编解码上下文；解码器不持有 Demuxer 引用。

编码包通过 `av_packet_clone`、解码帧通过 `av_frame_clone` 持有 FFmpeg 引用计数缓冲，发布事件复制共享引用，不复制完整像素/PCM。背压时事件可以跨多个调度轮次保留。输出为共享只读 SDK Buffer，通过输出节点的 MediaChannel 交给应用线程。

节点初始化、启动、停止和释放统一由 GraphEngine 按下游到上游顺序执行，媒体准备按上游到下游顺序执行。初始化或准备失败都释放整图。Seek 递增内部 epoch，并清空各节点的输入/待发布事件、解码器、重采样器和输出队列；旧 epoch 事件会被丢弃。EOS 沿媒体边传播，解码器和重采样器完成尾部排空后才向下游发布 EOS，所有节点完成后图才返回 `end`。纯音频/纯视频仍保留固定拓扑，未使用的分支只转发 EOS。订阅回调的异常通过投递确认显式回传，避免 EventBus 日志捕获导致静默失败。`nodes()` / `edges()` 提供内部拓扑、生命周期和实际收发计数。

播放器时钟仍由 Player 场景引擎管理；Qt 图像呈现和音频设备提交仍在界面线程，消费图的输出节点。这里的输出节点是 SDK 帧出口，不是 Qt 设备节点。本次没有开放通用高级图 API、Runtime 设备管理或动态拓扑。

视频为 CPU RGBA8，音频为 48000 Hz / FL、FR / 交错 float32。Buffer 使用 `shared_ptr<const vector<...>>`，取出后可跨线程保留；保留帧不会随 Seek/close 悬空。所有时间归一化到容器起始时刻，单位为微秒。每次 open、seek、stop、close 更新 generation；Seek 清理解码器、重采样器、待交付帧与通道，丢弃目标时间之前的视频，并裁掉跨越目标时间的 PCM 前缀。

## API 与生命周期

```cpp
#include <livekit/player.hpp>
using namespace std::chrono_literals;

livekit::sdk::Player player;
auto info = player.open("D:/media/example.mp4").get();
player.play().get();
// 在应用的 UI / 输出循环中持续调用：
auto video = player.poll_video();
auto audio = player.poll_audio();
// 按输出格式将数据交给自己的显示与音频设备。
player.pause().get();
player.seek(1500ms).get();
player.play().get();
player.close().get();
```

`open/play/pause/seek/stop/close` 返回 future，表示操作实际完成，而非仅入队。无回调线程，公共方法线程安全，错误可通过 future 异常或 `snapshot().error` 获取；媒体工作线程失败会进入 `failed`。参数或状态错误抛出标准逻辑异常，保留现有会话；后端失败需要重新 open。

- `open` 仅接受本地常规文件路径；FFmpeg 协议白名单为 `file`，不开放网络 URL。
- `open` 后为 `ready`，尚未自动播放；`play` 后为 `playing`，`pause` 冻结时钟并停止常规帧交付。
- 暂停状态的 `seek` 在完成清理后异步准备一个目标视频帧，由 `poll_video()` 取出一次；`poll_audio()` 保持为空，时钟不推进。预览阶段静默消费音频避免队列背压，恢复播放前从目标位置重新解码，避免丢失音频。纯音频或文件末尾没有预览帧；新 seek/stop/close 会清除待交付预览。
- `seek` 保留正在播放状态，否则进入 `paused`；定位范围为 `[0, duration]`，未知总时长时只检查非负。多个 Seek 串行完成。
- `stop` 定位到零并回到 `ready`；`ended` 上调用 `play` 自动从头播放。
- 解封装 EOF 后排空两个解码器和重采样器；输出队列取空且最后一帧时间结束后才进入 `ended`。
- `close` 释放图和媒体资源，幂等，之后可再次 open。析构取消 I/O 并等待工作线程退出。
- FFmpeg I/O 支持中断和默认 10 秒操作超时；`cancel_pending_io()` 可显式中断当前操作。open/close 会请求中断正在进行的 I/O。Seek/stop 按命令队列串行执行，不取消前一个 Seek 的 future。无法中断不检查 interrupt callback 的底层内核调用或单次 codec 运算。

## 输出契约与首版边界

SDK 使用 steady clock 作为主时钟。`poll_video()` 返回当前应显示的视频；`poll_audio()` 允许最多提前 100 ms 取出 PCM，以供设备缓冲。应用必须消费文件中存在的两种媒体；有界队列满后暂停解码，仍可处理控制命令。默认输出队列每支路最多 8 个视频帧 / 64 个音频帧及 128 MiB。此外，Demuxer 最多保留一个待发布包，每个解码/转换节点最多保留一个输入事件和一个待发布事件，另有 FFmpeg 内部缓冲。编码包限制为 64 MiB，视频转换限制为 32M 像素，超过输出队列字节限额的单帧报告错误。输出队列字节限额不代表整个图的总内存上限。

暂停/Seek/stop/close 时，应用负责同步暂停或清理自己的音频设备、显示缓存。设备缓存不属于 SDK；跨控制操作保留的帧须检查 generation。当前 Qt6 应用已接入这些清理操作；暂停保留设备缓存，Seek/stop/close 清理设备缓存。慢消费者会产生播放迟到，示例只显示本轮最新到期视频。第一版不承诺音频硬件时钟主导的长期同步、无缝音频、实时低延迟或逐样本同步精度。

当前选择第一条最佳音频/视频流，软件解码；暂不包含网络播放、倍速、硬件解码、音轨切换、字幕、HDR 色调映射、旋转元数据处理和共享音频设备/AEC 协调。解码音频中途变更格式会明确报错。Windows x64 为本次验证平台，其他平台尚未验收。

## 验证

以下 2026-10-26 记录为历史 Windows/SDL 验证。当前 Qt6 应用的构建方法和自动化测试归属见应用 README；历史结果不代表 Qt6 Windows 版本已经验收。

2026-10-26：Windows x64 / MSVC 19.44 / FFmpeg 7.1（`b08d796`）Release 构建通过；接入 GraphEngine::loadFromNodes 后 CTest **7/7 通过**。`playback_graph` 验证实际媒体订阅、收发计数、节点/连接、生命周期、输出背压、Seek 清空与代次、解码排空、重复播放、失败回滚、取消和 Buffer 生命周期。`media_flow` 通过 GraphEngine::loadFromNodes 加载独立的生产/处理/消费节点，验证引擎总线归属、节点和边总线释放，以及乱序注册后的拓扑调度、拒收重试、顺序、EOS、旧 epoch 丢弃、处理异常回传和共享数据所有权。保留 ImGui 空窗口、实际视频纹理渲染、异步暂停/Seek/停止、切换文件、错误恢复、窄窗口和打开过程关闭的回归测试。播放器继续独立于 WebRTC 构建。

CTest 生成短媒体并验证：真实解封装/解码、B 帧尾部排空、44.1→48 kHz 重采样尾部、音画单流、非零起始时间、暂停冻结、连续 Seek 与旧代次隔离、EOS 重播、有界队列、关闭幂等、多实例隔离、错误路径、中文路径，以及 SDL dummy 音视频输出闭环。SDL dummy 测试不代表真实扬声器和显示器已人工验收。

迁移后 SDK CTest 保留 `player_fixtures`、`local_player`、`playback_graph`、`media_flow` 四项；应用 CTest 在 livekit-app 中运行，以上 7/7 为迁移前历史记录。
