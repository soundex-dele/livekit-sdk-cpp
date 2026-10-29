> 历史接口记录：本文的旧 Room/Participant/帧流 API 已删除，不适用于当前构建。当前用法与支持范围见 [编码媒体 API](rtc-encoded-api.md) 和 [实施清单](rtc-backend-refactor.md)。

# 逐轨 PCM 读取

`RoomOptions.audio_frame_stream` 输出已订阅远端轨道解码后的 PCM，不经过应用混音或音频设备播放。各块带 `track_id`，使用 `read_for(track_sid, timeout)` 可单独消费指定轨道；`read(timeout)` 按队列顺序消费任一轨道。多个读取线程竞争数据，不进行广播。

```cpp
using namespace std::chrono_literals;
auto audio = std::make_shared<livekit::sdk::AudioFrameStream>(
    livekit::sdk::AudioStreamOptions{50, 4 * 1024 * 1024,
                                   livekit::sdk::AudioSampleFormat::pcm16});
livekit::sdk::RoomOptions options;
options.audio_frame_stream = audio;
livekit::sdk::Room room;
room.join_with_token(url, token, options);
auto next = audio->read(1s);
if (next.status == livekit::sdk::AudioReadStatus::frame) {
  auto frame = next.frame; // shared_ptr<const AudioFrame>，独立拥有 PCM 数据
  // samples 为交错 PCM16；samples_per_channel 不包含声道数。
  // sample_rate、channels 以每个 block 的实际值为准。
}
room.disconnect();
room.wait();
```

选择 `AudioSampleFormat::float32` 时，`float_samples` 存放归一化到 [-1, 1) 的交错样本，`samples` 为空。PCM16 模式相反。转换使用原生 PCM16 / 32768，不改变采样率、声道或布局；本接口没有添加重采样或混音。`duration()` 为每声道样本数 / 采样率的微秒值。

可编译示例为 `examples/audio_stream/main.cpp`，构建目标 `livekit_audio_stream_example`。设置 `LIVEKIT_URL` 和入会 `LIVEKIT_TOKEN`，由另一客户端在同房间发布音频，即可无窗口读取；不访问麦克风或扬声器。

## 时间连续性与丢块

`sequence` 是同一轨道时间线中的块序号，`sample_index` 是每声道解码样本位置，均在入队前递增。慢消费者导致的丢块会表现为间隙；不能把剩余块直接拼接并声称时间连续。应用可选择补静音、重置自己的处理器或跳到最新位置；SDK 不自动补齐应用队列丢弃的音频。

`stream_generation` 在完整 rejoin 或 RoomMoved 边界变化，旧队列被清空；信令 resume 保留代次。轨道重建、采样率或声道变化会产生新的 `track_generation` 并重置样本位置；旧格式的待读块被清除。`format_changed` 标记新格式首块，该块也可能因背压被丢弃，因此应优先比较 generation 和块中实际格式。

`sample_index` 是解码后的样本计数，包含原生解码器可能生成的丢包隐藏样本，不是 RTP timestamp，也不证明原始网络包连续。`capture_timestamp_ms` 仅在 WebRTC 提供捕获时间估计时保留，使用 WebRTC TimeMillis 时钟；缺失时保持空值。它不是 UTC、C++ steady_clock 纪元或声卡实际播放位置。此接口不代表音画同步已经实现。

## 有界队列与关闭

默认全流最多 50 块、4 MiB 样本字节，最大配置为 1024 块、64 MiB。达到任一限制时丢弃最旧完整块，超大块直接丢弃；所有轨道共用这些限制，不保证低帧率轨道免于淘汰。`stats()` 的 dropped/dropped_samples 包括容量、格式和生命周期清理；dropped_samples 是各丢弃块的每声道样本数之和，不是统一采样率的时长。

采样数据通过媒体图的 `AudioFrameOutputNode` 从原生 audio sink 复制进入队列，没有应用回调或等待消费者取走数据。队列用短临界区保护，并非无锁或硬实时接口。字节上限只约束排队样本；原生解码器、临时复制和应用已取走的帧不在该上限内。

`read(0ms)` 轮询，正超时最多 24 小时。可传入 stop_token 取消读取，取消优先且不消费已有块。移除轨道会清除该轨道的待读块；等待指定轨道的 read_for 可继续等到重新订阅或超时，轨道退出由 Room 回调通知。

stream 单次绑定一个逻辑 Room 会话；不能跨 Room 共用或关闭后重新打开。`close()` 清空并唤醒读取者，但不离开 Room。Room 终止或连接失败会关闭输出，已返回的不可变样本继续有效。应用须保证对象在读取方法调用期间存活，也不能在会话串行回调中阻塞等待读取。

可以同时启用音频和视频 stream；两者队列独立。该接口目前仅为远端解码输出，不提供本地 PCM 输入、混音输出、硬件音频时钟或音画同步。自动化使用合成音调和无窗口接收，不代表物理音频设备人工验收。
