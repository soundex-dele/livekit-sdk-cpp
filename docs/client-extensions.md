# RTC 客户端扩展

本批实现只涉及 RTC 客户端；Go 对照为 server-sdk-go 2.19.0 / protocol v1.53.0。默认信令仍为 12，支持显式选择 15、17；single_peer_connection 必须使用 17。构建和互通结果单独记录，源码接口存在不代表所有服务端及设备已验收。

## 使用入口

- 文本、字节流及文件：LocalParticipant 的 stream_text、stream_bytes、send_text、send_file，Room 的 register_text_stream_handler/register_byte_stream_handler。Reader 在独立工作线程调用，支持增量读取、停止和关闭；文件名仅为元数据，不自动写入磁盘。write/close 完成表示本地通道提交，不是接收确认。异常结束、断线、取消后不重放文件。接收支持 raw-DEFLATE，发送采用未压缩分块；operation/version 为应用元数据，不支持对已经读取的 chunk 做追溯编辑。
- DataTrack：publish_data_track、Room::remote_data_tracks、RemoteDataTrack::subscribe、LocalDataTrack::write。使用独立不可靠通道，保留最新完整待发帧；每帧上限 1 MiB，缺片 500 ms 过期。resume 丢弃传输中帧；rejoin 使旧本地句柄失效，调用 local_data_tracks 获取恢复后的句柄，已有 reader 按发送者和名称恢复订阅。
- 编码媒体：MediaSpec::encoded_source 输入 VP8、H.264/H.265 Annex-B access unit 或 Opus；EncodedMediaStream 输出解密后编码帧，decode_video=false 可跳过视频解码。队列有界，关键帧恢复通过 source.stats().keyframe_required 查询。H26x 必须包含可解码的参数集；不接受 AVCC 或带 B 帧乱序的文件。EncodedFileReader 需要 LIVEKIT_RTC_FFMPEG_CODECS，仅解复用及格式转换，不解码重编码。
- RTP：PacketStream 输出 RTP 头/扩展和完整 RTCP，RtpClock 根据 SR 映射时间；TrackPublication::request_keyframe 在下一帧处理时请求关键帧。RtpMediaSource 接受有序 RTP 并重组编码帧，原生 WebRTC 重新分配 SSRC/PT/序号/扩展；不提供逐字节 RTP 重放或任意 RTCP 注入。
- Simulcast：TrackPublishOptions::video_encodings 配置 1–3 层原始 VP8，按分辨率从低到高排列，RID 唯一。各层实际独立编码；两层对应 LOW/HIGH，三层对应 LOW/MEDIUM/HIGH。
- 备用编码：backup_codec 为同一原始视频源按服务端订阅需求启动第二路编码，使用同一 publication SID。支持 prefer_regression、simulcast、regression；媒体加密与备用视频组合当前拒绝。音频仅支持 Opus 发布，本地不支持音频备用 codec。
- E2EE：ExternalKeyProvider、FrameCryptor、RoomOptions::encryption 和 TrackPublishOptions::encryption_keys。数据、DataTrack、Opus/H.264/H.265 使用 Go 兼容 AES-GCM 格式。轮换使用新 key index，并保留旧 key 供在途帧解密；密钥分发由应用完成。原生 H26x 发送先统一 Annex-B 起始码，保证认证头在 RTP 重组后保持一致。加密 H.264 关键帧须包含 SPS/PPS/IDR。VP8 媒体 E2EE 不支持。
- 同步和统计：synchronize_av 使用实际混音器拉取的音频采集时间及播放延迟驱动视频呈现，缺失音频时回退本地时钟。graph-track-stats 报告图内解码、呈现与丢弃；WebRTC stats 主要用于传输。实际设备音画同步需要单独验收。
- 原始输入：I420SourceOptions::dynamic_dimensions 允许偶数尺寸变化；PCM 允许非 10 ms 整数块，内部拼成 10 ms。drift_compensation 根据调用方单调 capture_time 估算微小漂移，max_drift_ppm 限幅；不用于纠正丢包或大时间跳变。
- control_callback_thread 使用有界顺序执行器，队列溢出或回调异常结束会话。不得在回调中调用 wait；可以 request_stop。log_sink 包含 SDK 事件，可能包含用户消息，应用应按自身日志策略保存。

## 资源与生命周期

流最多 32 个并发 reader/writer，单 reader 缓存 1 MiB、单 writer 256 KiB、发送会话 2 MiB。媒体、DataTrack 与观察队列分别有界；慢消费者不阻塞原生网络线程。外部编码源单发布租约，完整重连清空旧代次帧。文件 helper 使用操作总期限。IPv6-only 与传输策略由原生网络管理和 ICE 配置共同执行。

## 验证入口

- client_extensions：流 UTF-8/压缩/公平调度/取消，DataTrack 乱序、重复、加密及重连，AES-GCM 标准向量、媒体格式与轮换，RTP 回绕、编码源代次、PCM 拼帧、控制执行器。
- media_graph：真实 VP8 三层编码及每层独立解码、按层停编码，既有图和媒体回归。
- tests/go_interop_integration.py --extensions plain|encrypted|single|backup|encoded|media-e2ee：固定官方服务端上的 C++↔Go 双向文本、300 KB 文件和 40 KB DataTrack 内容校验。媒体模式分别验证单 PC 预发布、H.265→H.264 按需备用编码、H.264 文件原码发送及 Go 回送解码、双向媒体 E2EE；每次结果记录二进制 SHA-256。
- protocol_contract：裁剪 schema 与固定上游 descriptor 严格对比。

Linux/macOS、Cloud 区域迁移、硬件编解码和实体设备音频播放均需要目标环境验收，不能从 Windows 自动化结果推定。

## 范围说明

能力查询的 supported 表示存在本地实现，仍受本文的 codec、连接模式和资源上限约束。原始 RTP 包的逐字节重放、任意 RTCP 注入、Pion 原生 interceptor/pacer 对象和 DTLS 曲线选择未作为公共 API 暴露。编码帧的任意 fmtp、帧 ID 和扩展用户时间戳也不承诺透传；DataTrack 的 user_timestamp 独立支持。以上不能由 encoded_media/rtp_access 的 supported 标记推定为 Go 每个底层接口均已等价。
