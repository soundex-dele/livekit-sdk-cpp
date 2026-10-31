# 最简 RTC 控制接口

本版只实现正常入会、轨道发现、显式发布/订阅、媒体收发、取消发布/订阅和退会。`RoomEngine → RoomObserver → RtcEngine → RtcCallbacks` 是唯一控制结果通知链路。

## 接口与时序

- `join/publish/subscribe/unpublish/unsubscribe/leave` 返回 `void`。返回不代表网络操作完成。
- `RoomObserver::on_joined` 在后端连接完成后触发，参数包含该批次的初始 roster。
- 后端提供接收端点后触发 `RoomObserver::on_subscribed`。RtcEngine 创建并启动对应 GraphEngine，然后触发 `RtcCallbacks::on_subscribed`。不等待首帧。
- `on_track_published` 表示发现远端轨道，上层明确调用 `subscribe(id)`、`subscribe(descriptor)` 或 `subscribe_media(spec)`。
- `on_published/on_subscribed` 携带 `MediaPublication/MediaSubscription`。纯编码图的 `track` 为空，媒体图的 `track` 可用于快照查询。
- `on_event` 传递轨道发现/移除、roster 和统计等持续事件。
- 取消单轨时先停媒体图，再解除后端端点。只有原生 detach 完成后才发取消成功回调；其它轨道和房间连接继续运行。
- `leave()` 先停全部媒体图，再停止后端、关闭端点，最后调用 `on_left`。之后可以显式重新 join。`close()` 是 leave 的便捷别名，不再引入永久终止状态。

## 驱动与生命周期

底层通过 Transport 事件回调主动通知 RoomEngine，RoomEngine 直接调用借用的 Observer，RtcEngine 启动媒体图后调用上层回调。RoomEngine 直接持有 Transport、Observer、房间状态和轨道表，不使用 PImpl。

不提供 `poll()` 或事件提取接口，也没有 RTC 控制线程或轮询定时器。LiveKit 适配器复用原有网络会话线程，在收到信令、媒体就绪、命令完成等通知后推送结果；空闲时等待通知，仅协议心跳、统计和业务超时保留各自的截止时间。

发布帧写入后立即发起发送，遇到背压后由底层 writable 通知触发重试；订阅帧到达后直接写入接收端点。订阅就绪时请求首个关键帧，后续显式请求直接转发。取消发布/订阅的完成通知由底层解除绑定回调触发。

控制层不处理线程安全。成功通知沿底层通知线程执行；`on_left` 在 `leave()` 调用线程执行。允许在入会或发现回调中发起发布、订阅；不要在底层回调中等待另一个结果、退会、替换回调或销毁引擎。Observer、回调捕获对象须存活到后端停止。UI 等上层代码按自己的线程要求转发通知。媒体帧回调继续按媒体节点原有模型执行。

```cpp
auto rtc = create_livekit_engine(AudioOutputOptions{});
RtcCallbacks callbacks;
callbacks.on_joined = [](const RoomSnapshot&) {
  std::cout << "joined\n";
};
callbacks.on_track_published = [&](const TrackDescriptor& track) {
  MediaSubscribeSpec spec;
  spec.track = track;
  spec.output.playout = true;
  rtc->subscribe_media(std::move(spec));
};
callbacks.on_subscribed = [](const MediaSubscription& track) {
  std::cout << "subscribed: " << track.id << '\n';
};
rtc->set_callbacks(std::move(callbacks));
rtc->join(options);
// 入会和订阅成功会由底层主动回调；保持 rtc 存活。
// 应用结束会话时（不在底层回调内）：
rtc->leave();
```

完整示例见 `examples/automatic_session/main.cpp`。编码自定义图继续通过 `graph_api.hpp` 提供，并共用 RtcCallbacks。

## 简化边界与迁移

移除 RtcExecutionContext、AsyncControl、LifecycleExecutor、OperationGuard、TrackGraphRegistry，以及逐次 Completion / Future 等待桥接、控制锁和异常转换。媒体构图复用已有的 RtcMediaGraphBuilder 和 GraphEngine；不增加新的 Builder 接口。

不提供重复请求校验、并发取消、超时结果、失败回调、异常捕获、失败图回收和自动重连恢复。调用者按正常顺序传入有效配置，回调和底层操作的异常不在控制层捕获。LiveKit 默认关闭自动重连；需要重连时由应用显式 leave/join。本版也移除自动 codec 回退重发布，按媒体配置选定 codec。

RoomView 的只读入口合并到 `snapshot()/services()`；修改参与者和订阅权限等命令直接从 RtcEngine 转发到 RoomEngine。服务本身的数据/RPC/流接口保持原有契约。底层 WebRTC、设备、端点帧队列和媒体节点的并发/错误机制不是本次控制层简化范围。

`support/rtc_blocking_bridge.hpp` 是未安装的应用/历史测试适配器，仅用条件变量等待底层主动通知，不驱动引擎；SDK 公共接口不依赖它。`RtcEventMailbox` 将通知转发给旧 supervisor。新应用应直接采用上面的回调写法。

## 自动化验证

`room_engine_tests.cpp` 分别以纯后端和 GraphEngine 模式编译，覆盖延迟 ACK、通知线程、roster 可见性、同名收发轨道、编码帧收发、图启动早于订阅回调、原生 detach 完成通知、单轨关闭、退会及显式重入。

公共头文件测试检查 void 控制接口且不引入 GraphFlow 头文件依赖。架构检查防止核心控制层重新引入锁、线程、Future、Completion 和 catch。媒体测试保留多轨、原始音频/编码输入、共享采集和显式重入的正常流程；旧的并发取消、异常恢复和永久 close 测试按已移除的契约撤销。

验证使用编译及无窗口测试，不操作桌面。自动化通过不代表真实摄像头、麦克风、扬声器或显示设备已人工验收。

## 2026-10-31 主动回调与直接成员版本验证

- RoomEngine 的状态、Transport、Observer、端点表直接声明在类中；架构检查禁止重新引入 `Impl/impl_`。
- 公共头文件检查确认 RoomEngine 和 RtcEngine 均没有 `poll()`。模拟后端在通知线程主动回调，业务线程只等待通知线程结束，验证成功通知无需外部驱动。
- 后端测试覆盖写入即发送、背压后 writable 回调恢复、订阅首个关键帧、显式关键帧请求、接收队列溢出后主动请求关键帧，以及解除绑定完成后立即回调。
- `rtc_backend`、`rtc_pipelines`、`rtc_dependency_boundaries`、`rtc_architecture`、`rtc_media_engine`、`cli_help` 六项通过。
- RTC 相关目标、公共头文件、自动会话/普通房间/音视频 source 与 stream 示例、CLI 和原生媒体集成客户端编译通过。
- 本地隔离 C++ SFU 集成客户端通过条件变量等待主动回调，不驱动引擎：2 次入会、2 次发布、2 次订阅、2 次退会通知，33 个解码音频帧和 10 个解码视频帧；NetEq 10 ms 输出和退会后帧存储检查通过。报告：`out/rtc-livekit-integration/20261031-100328-s5gi_u7j/result.json`。
- 本轮使用 `--observer-only`，没有验证发送端采集时钟扩展或 A/V presentation 互通，也没有进行真实设备人工验收。源码格式和 `git diff --check` 通过。

## 2026-10-31 初版验证记录（主动回调改造前）

- 使用 macOS x86_64、Homebrew LLVM 22.1.2 构建；新增及重写 C++ 文件按 clang-format 20.1.8 校验。
- 纯后端配置的 `result`、`async_result`、`rtc_backend`、`rtc_pipelines`、`rtc_dependency_boundaries` 通过。
- 原生配置的 `rtc_backend`、`rtc_pipelines`、`rtc_dependency_boundaries`、`rtc_architecture`、`rtc_media_engine`、`cli_help` 通过。
- 首次关键帧请求遗漏修正后，`rtc_media_engine` 连续运行 5 次通过；新增后端断言检查新视频订阅主动请求一次关键帧，空闲 poll 不重复请求。
- 编译通过：RTC 后端、流水线、媒体引擎、公共头文件检查、普通/自动会话示例、音视频 source/stream 示例、CLI 和原生媒体集成客户端。
- 本地隔离 C++ SFU 普通收发测试通过：2 次入会、2 次发布、2 次订阅、2 次退会通知，35 个解码音频帧和 10 个解码视频帧；验证 NetEq 10 ms 输出和退会后帧存储有效。报告位于 `out/rtc-livekit-integration/20261031-073943-fj7qqj4i/result.json`。

复现原生 Observer 测试（已有匹配的 SFU 可执行文件）：

```sh
python3 tests/rtc_livekit_integration.py \
  --client out/rtc-simple-native/Release/livekit_rtc_media_integration \
  --server ../livekit-cpp/out/conference-macos/livekit-server \
  --server-kind cpp --observer-only
```

`--observer-only` 明确不要求发送端采集时间扩展。默认完整时钟模式保留原有断言：本地旧 SFU 能完成入会和媒体收发，但不提供该扩展，测试报告 `timed=0,0` 而失败；本次不宣称这项时钟互通已通过。结果 JSON 对 `senderClock`、`avPresentation` 均记录 false。

整个 SDK 的无目标全量构建另外遇到既有 macOS 远程控制文件 `src/backends/platform/macos/control_input_mac.mm:24` 缺少 `screen_capture_devices` 声明；该文件未修改。上述 RTC 相关目标独立构建通过。测试没有启动真实采集/播放设备，没有进行桌面操作或设备人工验收。
