> 历史接口记录：本文的旧 Room/Participant/帧流 API 已删除，不适用于当前构建。当前用法与支持范围见 [编码媒体 API](rtc-encoded-api.md) 和 [实施清单](rtc-backend-refactor.md)。

# 有界视频帧读取

`RoomOptions.video_frame_stream` 提供独立于应用处理速度的 I420 pull 输出。应用在自己的线程调用 `read`，SDK 不调用用户媒体处理函数；消费者暂停读取不会阻塞 Room 信令与控制事件。现有 `video_frame_handler` 保留串行回调语义，两者不能同时设置。音频 PCM 使用独立的 [AudioFrameStream](audio-stream.md)；原始帧输入及编码帧接口尚未提供。

```cpp
using namespace std::chrono_literals;
auto frames = std::make_shared<livekit::sdk::VideoFrameStream>(
    livekit::sdk::VideoStreamOptions{3, 16 * 1024 * 1024});
livekit::sdk::RoomOptions options;
options.video_frame_stream = frames;
livekit::sdk::Room room;
room.join_with_token(url, token, options);

std::stop_source cancel_read;
auto next = frames->read(1s, cancel_read.get_token());
if (next.status == livekit::sdk::VideoReadStatus::frame) {
  // shared_ptr<const VideoFrame>；拥有像素，离开 Room 后仍有效。
  auto image = next.frame;
  // 应用在此线程读取 image->y/u/v、stride、track_id、stream_generation。
}
room.disconnect();
room.wait();
```

可编译示例在 `examples/video_stream/main.cpp`，构建目标 `livekit_video_stream_example`。使用环境变量 `LIVEKIT_URL` 和已有入会 Token `LIVEKIT_TOKEN`；另一客户端向同房间发布视频即可无窗口接收。

## 队列与时间

- 默认最多 8 帧、32 MiB 像素；帧数可设 1–1024，像素字节上限可设 1 byte–512 MiB。达到任一上限就丢弃最旧的完整帧；单帧超过字节上限直接丢弃，不破坏队列里已有帧。
- `read(0ms)` 轮询，正超时最多 24 小时；返回 frame、timeout、cancelled 或 closed。已取消的读取优先返回 cancelled，不消费队列。取消一个读取不关闭流。
- `stats()` 报告 queued_frames/queued_bytes、received、dropped、generation 和 closed。received/dropped 统计进入 pull 输出后的帧，包括容量淘汰和生命周期清理；原生接收层在每轨最新帧槽中替换的帧不在该计数内。
- 字节上限只计算队列中 Y/U/V 像素，不包括解码器、转换过程、原生每轨最新帧槽和应用已经取走并保留的帧。应用应自行限制保留量。
- 输出是旋转校正后的 CPU I420，保留原生 `timestamp_us`。该时间不是统一的音频播放时钟，不据此宣称音画同步完成。CPU 转换仍在 SDK 会话线程执行，此接口隔离的是应用消费者等待与处理。
- 多读取线程竞争同一队列，不是广播；frame.track_id 区分轨道。多轨共用容量，低帧率轨道可能被高帧率轨道的更新淘汰。

## 生命周期

每个 stream 只能绑定一个逻辑 Room 会话一次，不能同时分享给多个 Room；新的 join 会话需要新建 stream。完整 rejoin 保留 stream 对象，清除排队旧帧并增加 `stream_generation`；信令 resume 保留原传输和代次。房间迁移也会增加代次，丢弃迁移前排队及转换中的旧帧。代次代表 SDK 的传输/房间边界，不是像素来源或硬件采集时间的认证。

轨道移除会清理该轨道的待读帧。已经返回给应用的不可变帧继续有效，应用应按代次决定是否丢弃自己持有的旧帧。默认输出远端已订阅视频；`MediaSpec.local_preview` 同样进入该流。

`close()` 丢弃所有待读帧并唤醒读取者，之后不可重新打开；只关闭输出，不离开房间。Room 停止、连接失败或生命周期结束会关闭流。不得在会话串行回调里阻塞 `read`，也不能销毁仍被其它线程调用方法的对象。保持 stream 的 shared_ptr 存活直到读取线程退出。

自动化使用合成视频、官方服务端及无窗口读帧，不能替代实际摄像头、显示器或音画同步验收。
