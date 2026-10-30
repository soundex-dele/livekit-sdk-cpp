# 房间视频统一回退

`RtcEngine` 支持可选的房间级 H265 → H264 策略。任一成员声明仅能解码 H264，或没有有效的能力声明时，所有参与策略的发布端停止 H265 发布并重新发布 H264。回退后不因成员离开而恢复 H265；本地 `leave()` 后开始的新会话重新决策。自动重连保留已有回退决定。

这是一套 SDK 发布端协作协议，SFU 仍只负责转发。房间中的**所有视频发布端必须启用策略**；旧版或其他语言客户端需要实现相同协议。服务器不会强制不参与策略的发布端改变编码。

## 启用

```cpp
LiveKitRoomOptions options;
options.url = url;
options.token = token;
options.signal_protocol = 15;
options.video_codec_policy = RoomVideoCodecPolicy{.can_decode_h265 = true};
engine.join(options, on_join);  // on_join 接收 Result<void>，不要在通知线程等待。
```

仅支持 H264 的客户端设置 `can_decode_h265 = false`。该选项声明本机真实能力，不会为设备增加编解码器。参与者必须能解码 H264，视频发布端必须能编码 H264。普通 `RoomEngine` 不拥有媒体图，会明确拒绝此选项；请使用 `RtcEngine`。

LiveKit Token 必须授予 `canUpdateOwnMetadata`，并使用支持属性更新确认的协议 15 或更高版本。SDK 在加入完成前公布能力；公布失败则退出房间并返回加入失败。Token 签发端应同时写入初始 `attributes`，避免新成员在首次属性更新之前被当作未知能力，从而保守回退：

```json
{"attributes":{"av.video.decode_codecs":"h264,h265"}}
```

H264-only 的值为 `"h264"`。值严格区分大小写；缺失、非法值以及只声明 H265 都按需要 H264 处理。`av.video.h264_latched="1"` 表示本房间已经回退。每个参与者都保留并传播该标记，新发布端即使支持 H265，也会从现有成员继承回退。应用不要覆盖这两个保留属性，也不要在新房间 Token 中复用旧房间的 latch。房间全部参与者离开后的跨空房间持久化需要业务服务保存状态。

## 发布和接收

`publish_video` / `publish_media` 使用原始帧、摄像头、屏幕、共享输入、生成源或 FFmpeg 解码输入时，由 GraphEngine 自动重建 H264 编码流水线。策略开启时视频编码限于 H264/H265；音频不参与切换。新发布请求如果仍指定 H265，会在房间已回退时改用 H264。

切换会短暂中断视频。本地 TrackId、名称、来源、尺寸、静音、码率配置等意图保留，HEVC fmtp 被清除，H264 使用 SDK 默认的 constrained-baseline / packetization-mode 1。替换编码器产生新的关键帧。旧媒体图会先停止，摄像头可能重新打开，文件输入可能重新从起点读取。

每次自动替换通过 `PublicationObserver::on_video_codec_republished` 返回 `Result<MediaPublication>`。**服务器 SID 与 MediaTrack 句柄都会变化**；旧句柄保持关闭，应用应接收新句柄。接收端按普通 `remote_track_removed` / `remote_track_added` 事件解除旧订阅并订阅新轨道。SDK 不会将应用此前对旧 SID 的订阅意图擅自扩大到新 SID。针对服务器 SID 的外部业务映射也需要更新；SDK 使用本地 TrackId 的订阅权限会随重新发布重新映射。

`video_codec_policy()` 和 `RoomObserver::on_video_codec_policy_changed` 提供本端状态。`h264_latched` 表示房间策略意图，`transitioning` 表示本端切换，**不是所有远端均已切换完成的证明**。`failure` 保存最近一次策略失败直到退出或新会话；应用应结合每路替换结果处理失败。

## H265 透传源

SDK 不会把 H265 字节重标为 H264。对 `MediaInput::encoded_source`，可以提供替代输入：

```cpp
spec.h264_fallback_input = [device] {
  // 在串行控制线程调用，旧图已经停止。快速返回可用的 H264 输入。
  // 设备控制耗时较长时，应提前准备替代源；不要在此等待 SDK 控制回调。
  MediaInput input;
  input.encoded_source = device->h264_source();
  return input;
};
```

替代源也可以提供原始帧，让 GraphEngine 编成 H264。没有替代源、工厂抛出异常、编码器不可用或发布协商失败时，该路保持停止，替换回调返回具体失败，其他可切换的视频继续处理。应用可以修复源后重新调用 `publish_media`。直接 `publish` / 自定义 `graph().publish_graph` 无法由 SDK 重建编码器，启用策略时只允许发布 H264 视频。

## 自动化验证

启用 `LIVEKIT_RTC_FFMPEG_CODECS` 的测试构建提供：

- CTest `rtc_codec_policy`：真实媒体图编码配合可控传输，验证回退、静音保留、旧句柄关闭、重复通知、后续发布、未知能力、传播标记、透传替代源和失败路径。
- `tests/rtc_codec_policy_integration.py --server <SFU> --client <livekit_rtc_codec_policy_integration>`：启动独立 SFU，两个发布端四路 H265；加入 H264-only 声明的客户端后，验证八条接收链路都实际解码 H264 像素；该客户端退出后，验证后来发布的视频和后来加入的发布端仍使用 H264。

C++ SFU 使用同一运行器，加 `--server-kind cpp` 生成其原生配置。

集成测试的 H264-only 能力由客户端明确声明，不代表测试机器本身缺少 H265 解码器。以上均为无窗口自动化验证，真实设备切换和人工观感验收另行进行。

### 2026-10-30 验证记录

- Windows Release 编译通过。CTest `rtc_codec_policy`、`rtc_media_engine`、`rtc_pipelines`、`rtc_dependency_boundaries` **4/4** 通过，日志：`../../out/room-codec-policy-checks.log`。
- 策略单测还覆盖自动重连后重新公布 latch、回退保持、不同轨道的成功/失败替换通知及返回句柄。相关 UAV 验收报告脚本单测 **11/11** 通过；不把原先分开执行的 H264/H265 场景计作全体回退验收。
- Go SFU：`../../out/rtc-codec-policy-integration/20261030-151916-s3e8xp2a/result.json`。
- C++ SFU（`out/windows-parity/Release/livekit-server.exe`）：`../../out/rtc-codec-policy-integration/20261030-152019-cc6h947s/result.json`。
- 两个 SFU 均通过四路初始 H265、回退后的八条 H264 接收链路，以及后续发布和新增发布端的十二条 H264 接收链路。报告保存客户端/服务端 SHA-256；同一最终客户端 SHA-256 为 `b1abd34ce3e88ec943ff492b36bc9e0aff02c9ab991bb8423f3bff2cc800077e`。
- 首次媒体回归在阻塞适配器的回调重入场景超时。`support/rtc_blocking_bridge.hpp` 已为媒体输出与替代源工厂加上回调保护，使同步等待在回调内返回 `invalid_thread`；最终原有媒体回归通过。较旧 C++ `out/windows-x64` 产物曾在连接阶段关闭会话，未通过；成功结论只针对上面记录的 parity 产物。
