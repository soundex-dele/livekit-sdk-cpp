# 应用 I420 视频输入

`I420VideoSource` 让应用直接提交 CPU I420 帧，经现有媒体图、libwebrtc 编码和 RTP 传输发布。无需 FFmpeg，不通过 JSON Command 传递像素。示例：`examples/video_source/main.cpp`，目标 `livekit_video_source_example`。

```cpp
auto source = std::make_shared<livekit::sdk::I420VideoSource>();
livekit::sdk::MediaSpec media;
media.video_source = source; // 默认 640x360，fps=30
auto track = room.local_participant().publish_track(media).get();
// frame.width/height 必须匹配 source，三个 plane 使用正 stride。
auto result = source->write(frame);
// accepted: 已复制入队；backpressure: 丢弃此次实时帧；closed: 永久结束。
```

尺寸在创建 source 时固定，宽 2..3840、高 2..2160，均为偶数；`MediaSpec.width/height` 必须一致。源不能与文件、demo、loop、设备采集组合。`MediaSpec.fps` 为最大取帧速率，不会补帧；应用控制实际提交频率。I420 输入要求已转正，`write_rgba` 则提供 RGBA 转换及旋转；动态分辨率和 GPU buffer 尚未提供。

`write` 复制可见像素并去除行 padding；最末行可不带 padding，负 stride、缺 plane、尺寸不符会抛出 `invalid_argument`。调用返回后可复用或修改原始像素。调用期间应用不能并发修改所传入的帧。内部锁只保护复制和队列操作，不等待编码器、网络或队列空间；这不是无锁或硬实时 API。

默认以提交时的本地单调时钟标记，也可调用 `write(frame, captured)` 传入 `steady_clock::time_point` 外部采集时间，进入原生媒体图时换算到 WebRTC 时间基。显式时间在每次激活内严格递增，合法窗口为 now−5s..now+100ms；已超过 `max_age` 返回背压，不将历史帧作为实时视频发送。背压不推进已接受时间线，完整 rejoin/重新激活会重置时间线。`VideoFrame.timestamp_us/track_id/stream_generation` 是输出侧元数据，输入时忽略。文件 PTS、UTC 或设备时钟应先由应用映射到 steady_clock；接口不提供设备漂移补偿或实际音频播放时钟同步。

`write_rgba(RgbaInputFrame, captured)` 接收字节顺序为 R/G/B/A 的 CPU 像素，忽略 alpha，经固定 libyuv 的 BT.601 limited-range 整数转换得到 I420，再按 `VideoRotation` 顺时针旋转 0/90/180/270 度。旋转后的尺寸必须匹配 source；例如 180×320 输入旋转 90 度后匹配 320×180 源。输入使用正 stride，末行不要求 padding。转换在调用线程执行，临时转换缓冲不计入队列字节上限。

队列同时受 `max_frames`、`max_bytes` 和 `max_age` 限制。默认 3 帧、32 MiB、200 ms；字节上限只统计队列的紧凑像素，不包括调用方像素、正在消费的单帧和原生编码/传输缓冲。未激活、已停止或队列满时返回 `backpressure`，不会等待空间或覆盖旧帧。每次写入/消费时淘汰过期帧；没有后台清理定时器，闲置队列仍有容量上限，过期帧不会进入后续消费。

`publish_track` 的服务端确认可能早于媒体协商完成。协商完成后才接收帧，应用应处理初始 `backpressure`，也可通过 `stats().accepting` 查询。一次只能存在一个原生发布绑定，同一 Room 或不同 Room 重复发布同一 source 都会失败。复用 source 前先取消发布并等待完成。该排他绑定在完整 rejoin 重建时暂时释放；不要在重连期间将同一 source 改发到另一个 Room，否则原会话恢复可能因重复占用失败。

完整 rejoin 默认恢复已确认的源对象，递增 `stats().generation`；停止时清空旧帧，重新协商前拒绝输入，重新激活后只消费新提交帧。`MediaRestartPolicy::fail` 仍可禁止恢复。未确认源不自动重发；完整 rejoin 后旧 publication 句柄失效，使用稳定 `TrackInfo.client_id` 查找新句柄。仅信令 resume 保留现有源和代次。

`close()` 是永久 EOS：丢弃排队帧并唤醒等待消费者，后续写入返回 `closed`，不能再发布该 source。已经取出的帧可能仍在编码/网络中。close 不自动取消服务器 publication，也不离开 Room；应在结束时调用 `unpublish_track`。关闭但尚未取消发布的源不能在完整 rejoin 中恢复。普通取消发布/Room 停止会释放绑定并清空队列，但不永久关闭 source；可在之后重新发布。`stats` 提供 accepted/consumed/dropped/rejected、队列大小和绑定状态。

验收分离：单测验证队列、stride、所有权和唤醒；官方服务端集成验证真实视频、拥塞时控制操作、rejoin、取消及再次发布。Go 互操作、外部音视频时钟同步和真实设备验收不能由本机 C++ 合成媒体结果替代。
