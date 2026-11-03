# MediaEngine

`livekit::sdk::client::MediaEngine`（`livekit/media_engine.hpp`，`LiveKit::MediaEngine`）在新同步 Room 上管理媒体流水线。采集、编解码、预览、播放和录制使用既有媒体节点与 GraphEngine；传输桥只使用 client::LocalTrack / RemoteTrack，不使用旧 RoomEngine、Endpoint 或 BlockingRtc。

公共接口遵循 Room：connect 返回 bool，创建媒体发布/订阅返回 Result<T>；close、撤销、静音、接收需求、数据发送、成员更新和 Observer 注册返回 void，失败在所在函数直接打印日志。内部可失败操作使用 Result<void>。保留 RoomObserver，事件线程不能重入同步控制；媒体输出回调同样只能查询快照。输出回调是持续媒体数据回调，不是操作完成回调。

MediaPublishSpec 使用新 Room 的 CodecFormat、TrackPublishOptions 和 QueueLimits。MediaInput/MediaOutput、原始样本与编解码选项复用既有媒体值类型。它们不创建旧 RTC 引擎。发布先验证/初始化图，再发布 Room Track，启动媒体；失败撤销发布。编码访问单元在媒体节点与 Room 独立值类型之间复制，两个方向都有有界队列。

订阅指定本 Room 的 RemoteTrackPublication。根据 roster 中的 MIME 选择解码器，解码后分发应用样本、共享音频设备和独立录制支路；同一成员/stream 的音视频可共享呈现时钟。读取和关键帧请求由媒体线程执行，不在 Room Observer 内执行同步请求。

本地媒体句柄跨全量重连保留，反馈携带当前 epoch 和静音/编码需求；已经进入编码器的旧 epoch 帧不会改标为新 epoch。远端重连产生新 Publication，由应用根据新 roster 建立订阅。close 先取消 Room 阻塞操作，再停止所有采集、编解码、读帧和输出线程。MediaTrack 快照在关闭后仍可查询。

会议 demo 使用 Session 工作线程调用 MediaEngine，Qt 线程不执行 connect/publish/subscribe。聊天和成员状态直接使用新 Room 数据包、Observer 和成员句柄。新 Room 使用 SFU 下发的 refresh_token 重连，也支持同步 TokenProvider。RPC、流、数据轨和 E2EE 通过相同的 ConnectionOptions 与参与者接口使用，见 [房间服务](room-services.md)。自定义媒体图通过下述可选接口提供；当前不提供共享采集句柄、H265 自动降级和订阅权限管理接口。

MediaEngine 撤销本地发布与静音使用 publication.snapshot().track_id；撤销订阅和接收选项使用远端 SID。MediaTrackSnapshot.frames 统计已交给 Room 的编码帧或从 Room 读出的编码帧，bytes 统计相同边界的编码字节数，input 提供输入节点完成状态；细分节点计数在 nodes 中。

SDK 的 automatic_session、video_source、audio_source、video_stream、audio_stream 和 conference_viewer 使用 MediaEngine；room_client 和 basic_room_encoded 只演示编码数据，直接使用 Room。web_viewer 使用 JavaScript SDK，不涉及 C++ 引擎。旧引擎已移除，其他原生应用也已迁移到 MediaEngine。

statistics 同步返回两条 PeerConnection 的 RTCStatsReport JSON、采样时间和本地轨道 ID 到 SID、远端 SSRC 到 SID 的映射，不暴露 WebRTC 类型。MediaTrack 快照提供采集、编码、收发、解码节点计数；会议示例保留逐轨道统计和订阅需求显示。ConnectionOptions 可选择 automatic、udp_only 或 tcp_only。

## 自定义媒体图

包含 `livekit/media_graph.hpp`，链接 `LiveKit::MediaEngineGraph`，在 `MediaPublishSpec::graph` 或 `MediaSubscribeSpec::graph` 设置 `shared_ptr<const MediaGraphOptions>`。普通 `media_engine.hpp` 仅前置声明该配置；Room 的头文件与链接依赖仍不包含 GraphEngine。

`MediaGraphOptions::build` 在 publish_media/subscribe_media 内同步调用一次，返回 `Result<MediaGraph>`，没有完成回调。传入的 MediaGraphContext 包含轨道描述、方向、默认图以及 port/input/codec/output 节点名称。默认图还未初始化，可在默认连接上插入水印、滤镜、音频处理节点或分支，也可保留 Room 端口后替换全部采集、编解码和输出节点。没有 graph 配置时保持默认流水线。

```cpp
auto options = std::make_shared<client::MediaGraphOptions>();
options->build = [](client::MediaGraphContext context) {
  auto graph = std::move(context.defaults);
  auto filter = std::make_shared<MyVideoFilter>("filter"); // 派生 GraphNode
  const auto type = graphflow::core::detail::prettyName<rtc::nodes::VideoFrameEvent>();
  for (auto& edge : graph.connections) {
    if (edge.from == context.input && edge.dataType == type) {
      edge.from = "filter"; // 编码和预览都接收处理后的帧
    }
  }
  graph.nodes.push_back(filter);
  graph.connect<rtc::nodes::VideoFrameEvent>(context.input, "filter");
  return Result<client::MediaGraph>::success(std::move(graph));
};
spec.graph = options;
auto publication = engine.publish_media(spec);
```

可编译的水印节点与生成视频发布示例位于 `examples/custom_media_graph/`，目标为 `livekit_custom_media_graph_example`。节点在修改 I420 数据前复制原始像素，保留时间戳、epoch 和其他媒体元数据；不能原地修改其他分支共享的样本。订阅端可用相同方式在 decoder 后插入处理节点。

图的约束和生命周期：

- 节点名称唯一、非空，所有节点必须处于 Created 状态且没有其他图的连接边。每次构图返回新实例；初始化过或释放过的节点不能复用。不同 MediaEngine 的节点校验与初始化串行化，避免同时接管同一新节点。应用不得并发把同一实例交给外部 GraphEngine。
- 必须保留 context.defaults 中的同一个 Room 端口节点。发布端接收 EncodedFrameEvent、发出 SenderFeedbackEvent；订阅端发出 EncodedFrameEvent、接收 KeyframeRequestEvent。发布图必须接通编码帧与反馈。Room 端口不接受条件路由。
- 所有连接必须有类型、端点存在且不能重复，节点必须连通至 Room 端口。除 SenderFeedbackEvent/KeyframeRequestEvent 外的数据连接须无环；最多 128 个节点、1024 条连接。实际节点支持的输入/输出事件类型仍由调用方保证。
- GraphEngine 管理 init/start/stop/release。SDK 自动激活图内的 MediaInputNode，并停止采集、编解码和输出节点。自定义生产者可用 `activate -> bool` 在全图 start 后开始生产，用 `quiesce -> void` 停止接收新工作、取消并等待其后台任务。quiesce 在部分初始化或启动失败时也可能执行，应能处理未启动状态；未通过校验的图不会调用钩子。
- build、activate、quiesce 和节点回调均不能重入 Room/MediaEngine 生命周期控制，不得抛异常。构图和生命周期钩子要及时返回；quiesce 未返回时 close 不会返回。生产线程上的回调必须快速处理，耗时工作由节点自己的有界队列和线程承担。GraphEngine 接管后，应用不得修改节点、连接、错误处理器或独立驱动节点生命周期。
- 构图错误直接返回 Failure；发布启动失败会撤销 Room 发布，订阅构图/启动失败会撤销订阅。节点调用 reportError 上报运行期失败，MediaTrackSnapshot.failure 保留诊断并停止媒体图；发布的撤销仍由 unpublish_track/close 管理。所有节点和连接出现在快照中，未知自定义节点的通用计数为零，细分计数可由应用节点自行提供。
- 自定义图固定于这次发布/订阅生命周期，不提供运行中热替换。重新构图通过撤销后重新发布/订阅完成。共享采集句柄属于另一项媒体能力。

## 自定义图验证记录（2026-11-03，Windows x64 Release）

- SDK 全量构建和应用当前 Qt6 配置构建通过，包含自定义图头文件、测试与水印示例；应用配置未启用独立 Player 桌面目标。
- `media_graph` 无窗口测试通过：完全自定义编码收发图、订阅关键帧请求、节点复用拒绝、静音反馈、关闭等待与单次释放、视频水印的真实 VP8 编解码、PCM 处理的真实 Opus 编解码、非法图校验、factory/init/start/activate 失败回滚及运行期错误诊断。
- SDK 34 项 CTest 中 32 项通过，默认环境下 2 项 SFU 测试跳过；应用 24 项中 22 项通过，remote_media 和 studio_media_session 因缺少外部媒体环境跳过。工作区日志为 `out/custom-media-graph-sdk-ctest.log` 与 `out/custom-media-graph-app-ctest.log`。
- 独立本地 Go SFU 上运行水印示例，5 个图节点成功发布 151 帧编码视频；记录为工作区 `out/custom-media-graph-sfu/result.json`。该项验证原生发布路径；像素水印和 PCM 处理的内容验证由上述真实 codec 回环测试完成。
- 268 个项目原生文件通过无异常约束检查；Room 依赖边界、旧引擎移除及 Git diff 空白检查通过。未进行真实设备人工验收。

## 基础 MediaEngine 验证记录（2026-11-03，Windows x64 Release）

- SDK MediaEngine、全部原生示例、SDL 会议示例，以及 Qt 会议应用和相关测试程序编译通过。新增 MediaEngine 业务代码关闭异常处理，第三方日志初始化保留独立适配边界。
- 9 项自动检查通过：livekit_client、livekit_client_dependencies、media_engine、no_exceptions_contract、demo_session、demo_render，以及应用的 conference_media_engine、conference_chat、conference_widgets。
- 真实本地 Go SFU：video_source/video_stream、audio_source/audio_stream、room_client、basic_room_encoded 和 automatic_session 共 5 组实际收发通过。运行器：`tests/media_examples_integration.py`；工作区证据：`out/media-sdk-examples-final/result.json`。
- SDK 会议示例通过多参与者、多轨道、逐轨道收发码率与帧率、动态发布、静音、录制、取消、重入会和服务端移除；证据：`out/media-sdk-conference-final/result.json`。
- UDP/TCP 两组订阅需求控制通过：无观众时采集、编码、RTP 计数停止，保留观众时继续发送，重新订阅恢复且 SID 不变；证据：`out/media-sdk-dynacast-final/result.json`。
- Qt 会议媒体与短令牌脱敏回归通过；证据：`out/media-app-redaction-regression/result.json`。聊天双向收发、房间隔离、Unicode 多行和离会重入通过；证据：`out/conference-chat/2026-11-03T01-06-14-524003Z/evidence.json`。
- Visual Studio 项目依赖闭包检查确认 MediaEngine 不链接旧 RTC backend、RtcEngine 或 RoomEngine；Room 仍不依赖 GraphEngine。修改的原生文件通过 clang-format 20.1.8 检查，Git diff 空白检查通过。

这些结果来自合成媒体、无窗口渲染和本地服务器，未进行真实摄像头、麦克风、扬声器人工验收，也未验证 macOS/Linux。

旧引擎及所有应用迁移见 [移除说明](legacy-engine-removal.md)。CLI、投屏、远控、直播工作台现也使用此接口。
