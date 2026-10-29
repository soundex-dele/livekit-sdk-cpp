> 历史接口记录：本文的旧 Room/Participant/帧流 API 已删除，不适用于当前构建。当前用法与支持范围见 [编码媒体 API](rtc-encoded-api.md) 和 [实施清单](rtc-backend-refactor.md)。

# 应用 PCM 音频输入

`PcmAudioSource` 接受应用 PCM，经独立媒体图、独立流式重采样器和原生 AudioTrack 发布。每条轨道有自己的源及 Opus 编码状态，不经过默认麦克风 ADM，也不会在 SDK 内将多条应用音轨混成一路。

```cpp
using namespace livekit::sdk;
auto source = std::make_shared<PcmAudioSource>(
    PcmSourceOptions{44100, 2, AudioSampleFormat::float32});
MediaSpec media;
media.video = false;
media.audio_source = source;
auto publication = room.local_participant().publish_track(media).get();
PcmInputFrame input;
input.float_samples.resize(882 * 2); // 20ms，交错 L/R
// 填入归一化样本。可选 capture_time 标识首个样本的采集时间。
auto status = source->write(input);
```

支持 8/16/24/32/44.1/48/96 kHz，单声道或双声道；格式固定为 PCM16 或 float32。一次写入包含 10..100 ms 的完整交错样本，时长必须为 10 ms 整数倍。PCM16 只填 `samples`，float32 只填 `float_samples`；浮点必须有限且在 [-1,1] 内，+1 饱和为 32767，-1 对应 -32768。首版内部转换为 PCM16 再以 WebRTC sinc 重采样到 48 kHz；每次送给原生音轨恰好 480 个每声道样本。修改格式需要新建源，不隐式改变进行中的滤波状态。

`write()` 复制样本，返回后应用可复用输入内存。SDK 不调用应用供帧回调，不等待编码器或队列空间。格式错误抛出 `invalid_argument`；没有激活、已停止或容量不足时返回 `backpressure`；永久 close 后返回 `closed`。`accepted` 只表示整次写入进入输入队列，不表示服务端或对端收到。背压时整次写入不产生任何样本或时间线变更，调用方可保留原块重试，或者明确丢弃它。

队列以 10 ms 块存储，受 `max_queue_duration` 和 `max_bytes` 双限额。默认最多 200 ms；时长可配置 10 ms..5 s，字节按内部交错 PCM16 计算。容量不包含应用持有的输入、正在构建的一次写入、正在处理的单块、重采样器和 Opus/SRTP 缓冲；多块写入在所有内存分配成功后原子接入。`stats()` 提供队列大小、accepted/consumed/dropped、silence/skipped 样本数及拒绝写次数；样本计数均以输入采样率下的每声道样本为单位。

时间线规则：

- 省略 `capture_time` 时，首块以本地 steady_clock 为基点，后续块接在已有样本后。欠载之后的下一次写入从当前输出位置继续，不回放欠载期间的历史块。
- 显式采集时间使用 `std::chrono::steady_clock::time_point`，代表首个样本；不得与此前已接收的样本时间重叠，不能混用 UTC、文件 PTS 或设备私有时钟。首样本时间必须在提交时刻前 5 秒至后 100 ms 范围内。时间在 10 ms 网格上按最近时隙处理（半格边界向后）；连续真实采集建议由同一基点加累计采样时长生成时间戳，避免每次读取系统时间的抖动造成重叠。
- 原生发送从首块可用时开始，之后按 10 ms 本地单调时钟节奏运行。时间戳用于样本缺口识别和采集时间元数据，不是指定首次发送墙钟时刻的调度 API。相邻显式时间有缺口时补静音，迟到块丢弃；两者分别计数，不通过拼接旧样本延长音频。
- 没有首块时等待，可取消。首块之后欠载会在原采样率补零并通过同一个滤波器；因此滤波器的短尾不等价于瞬时全零声压。应用没有恢复输入时，静音会持续至取消发布或 close。
- 工作线程落后超过 20 ms 时跳过过期时隙、清理过期块并重置滤波器，不无限追赶发送。`skipped_samples` 记录调度缺口；不承诺重载下无损音频或硬实时调度。

每个 PCM source 同时只能被一个发布绑定占用，重复发布或跨 Room 使用会失败。`publish_track` 返回服务端确认可能早于源激活，应用需处理初始背压。取消发布会释放绑定并清空队列，可随后重新发布。完整 rejoin 自动恢复已确认且未 close 的源，清空旧时间线/队列、重建滤波器、递增 generation；旧 publication 失效，使用稳定 `TrackInfo.client_id` 关联新句柄。不要在重连期间将同一 source 重新绑定到其它 Room。未确认源沿用不重放契约，`MediaRestartPolicy::fail` 仍可禁止恢复。

`close()` 永久结束源，清空输入并唤醒等待者；它不等于远端播放完成，也不取消服务器 publication。应显式取消发布；已 close 但尚未取消的源不能参与完整 rejoin。已取出的音频可能仍在编码器或网络中。`MediaSpec` 不允许 PCM source 与文件、设备、demo、loop、视频源或本地视频预览组合。

双声道通过对齐 Go SDK 的 `AddTrackRequest.stereo` 告知服务器，同时 SDK 的 Opus 解码工厂声明可接收立体声；具体是否保持立体声还取决于协商和服务器。单声道 Opus 也可能解码为左右重复的双声道输出，消费者必须读取 AudioFrame.channels，而非假定与发送源相同。采集时间按本地 clock 差换算为 WebRTC TimeMillis 后传入原生音轨；不将其当作声卡播放时钟、UTC 或跨机器共享时钟。真实音画同步、设备时钟漂移补偿仍属于后续阶段。

MediaSpec 新增成员，预编译消费者需要重新编译和链接。

可编译示例：`examples/audio_source/main.cpp` / `livekit_audio_source_example`。验证证据以 [实施记录](client-parity-progress.md) 为准；本机原生/官方服务端自动化不等同于 Go 互操作或物理设备验收。
