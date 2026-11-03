# MediaEngine

`livekit::sdk::client::MediaEngine`（`livekit/media_engine.hpp`，`LiveKit::MediaEngine`）在新同步 Room 上管理媒体流水线。采集、编解码、预览、播放和录制使用既有媒体节点与 GraphEngine；传输桥只使用 client::LocalTrack / RemoteTrack，不使用旧 RoomEngine、Endpoint 或 BlockingRtc。

公共接口遵循 Room：connect 返回 bool，创建媒体发布/订阅返回 Result<T>；close、撤销、静音、接收需求、数据发送、成员更新和 Observer 注册返回 void，失败在所在函数直接打印日志。内部可失败操作使用 Result<void>。保留 RoomObserver，事件线程不能重入同步控制；媒体输出回调同样只能查询快照。输出回调是持续媒体数据回调，不是操作完成回调。

MediaPublishSpec 使用新 Room 的 CodecFormat、TrackPublishOptions 和 QueueLimits。MediaInput/MediaOutput、原始样本与编解码选项复用既有媒体值类型。它们不创建旧 RTC 引擎。发布先验证/初始化图，再发布 Room Track，启动媒体；失败撤销发布。编码访问单元在媒体节点与 Room 独立值类型之间复制，两个方向都有有界队列。

订阅指定本 Room 的 RemoteTrackPublication。根据 roster 中的 MIME 选择解码器，解码后分发应用样本、共享音频设备和独立录制支路；同一成员/stream 的音视频可共享呈现时钟。读取和关键帧请求由媒体线程执行，不在 Room Observer 内执行同步请求。

本地媒体句柄跨全量重连保留，反馈携带当前 epoch 和静音/编码需求；已经进入编码器的旧 epoch 帧不会改标为新 epoch。远端重连产生新 Publication，由应用根据新 roster 建立订阅。close 先取消 Room 阻塞操作，再停止所有采集、编解码、读帧和输出线程。MediaTrack 快照在关闭后仍可查询。

会议 demo 使用 Session 工作线程调用 MediaEngine，Qt 线程不执行 connect/publish/subscribe。聊天和成员状态直接使用新 Room 数据包、Observer 和成员句柄。新 Room 使用 SFU 下发的 refresh_token 重连；不提供旧异步 TokenProvider，重新加入会议时由业务 API 重新获取凭据。当前不提供旧 RtcEngine 的自定义图、共享采集句柄、H265 自动降级和订阅权限管理接口。

MediaEngine 撤销本地发布与静音使用 publication.snapshot().track_id；撤销订阅和接收选项使用远端 SID。MediaTrackSnapshot.frames 统计已交给 Room 的编码帧或从 Room 读出的编码帧，细分节点计数在 nodes 中。

SDK 的 automatic_session、video_source、audio_source、video_stream、audio_stream 和 conference_viewer 使用 MediaEngine；room_client 和 basic_room_encoded 只演示编码数据，直接使用 Room。web_viewer 使用 JavaScript SDK，不涉及 C++ 引擎。旧引擎和其他应用仍保留。

statistics 同步返回两条 PeerConnection 的 RTCStatsReport JSON、采样时间和本地轨道 ID 到 SID、远端 SSRC 到 SID 的映射，不暴露 WebRTC 类型。MediaTrack 快照提供采集、编码、收发、解码节点计数；会议示例保留逐轨道统计和订阅需求显示。ConnectionOptions 可选择 automatic、udp_only 或 tcp_only。

## 验证记录（2026-11-03，Windows x64 Release）

- SDK MediaEngine、全部原生示例、SDL 会议示例，以及 Qt 会议应用和相关测试程序编译通过。新增 MediaEngine 业务代码关闭异常处理，第三方日志初始化保留独立适配边界。
- 9 项自动检查通过：livekit_client、livekit_client_dependencies、media_engine、no_exceptions_contract、demo_session、demo_render，以及应用的 conference_media_engine、conference_chat、conference_widgets。
- 真实本地 Go SFU：video_source/video_stream、audio_source/audio_stream、room_client、basic_room_encoded 和 automatic_session 共 5 组实际收发通过。运行器：`tests/media_examples_integration.py`；工作区证据：`out/media-sdk-examples-final/result.json`。
- SDK 会议示例通过多参与者、多轨道、逐轨道收发码率与帧率、动态发布、静音、录制、取消、重入会和服务端移除；证据：`out/media-sdk-conference-final/result.json`。
- UDP/TCP 两组订阅需求控制通过：无观众时采集、编码、RTP 计数停止，保留观众时继续发送，重新订阅恢复且 SID 不变；证据：`out/media-sdk-dynacast-final/result.json`。
- Qt 会议媒体与短令牌脱敏回归通过；证据：`out/media-app-redaction-regression/result.json`。聊天双向收发、房间隔离、Unicode 多行和离会重入通过；证据：`out/conference-chat/2026-11-03T01-06-14-524003Z/evidence.json`。
- Visual Studio 项目依赖闭包检查确认 MediaEngine 不链接旧 RTC backend、RtcEngine 或 RoomEngine；Room 仍不依赖 GraphEngine。修改的原生文件通过 clang-format 20.1.8 检查，Git diff 空白检查通过。

这些结果来自合成媒体、无窗口渲染和本地服务器，未进行真实摄像头、麦克风、扬声器人工验收，也未验证 macOS/Linux。
