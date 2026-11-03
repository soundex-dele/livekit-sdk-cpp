> 历史文档：旧 RtcEngine / RoomEngine 及底层实现已移除。本文仅记录当时设计或验证，不代表当前 API；参见 [迁移说明](legacy-engine-removal.md)。

# RTC 完成回调与持续 Observer

当前契约（2026-11-02 按源码核对）：控制方法保持 `void`，每次调用可传入 `Completion<T>`
（`std::function<void(Result<T>)>`）；持续状态通过 `RoomObserver` / `RtcObserver` 接收。
空 completion 表示调用方主动忽略本次结果，不再通过全局成功事件找回结果。

## 完成与事件

- join 的结果为 RoomSnapshot；connected 到达后取出并清空本次回调。后续 connected
  只产生持续事件，不重复交付 join 结果。初始会话信息在 join 成功时可读。
- RoomEngine 的 publish/subscribe 返回对应端点；RtcEngine 返回 MediaPublication /
  MediaSubscription，表示端点 ready 且图已启动，不表示远端首帧已显示。
- unpublish/unsubscribe 在原生端点解绑完成时结算；RtcEngine 先停止图，成功解绑后移除轨道条目。解绑失败不表示资源已全部释放。
- set_muted、接收需求和权限成功表示后端本地应用/信令提交；参与者更新等待协议响应。
- 本地校验、后端失败及媒体构图失败都通过 Result 交付；不以异常向控制调用者报错。
  重复结果不再次调用 completion；退会会以 cancelled 结算仍在等待的操作。
- 连接变化、远端轨道增删、会话/权限/统计及后端轨道故障使用持续 Observer。
  操作结果与状态事件可能由同一故障引起，它们用途不同。
- Observer 是借用对象，须存活到 set_observer(nullptr) 返回。替换 Observer 不取消
  已提交 completion。set_observer 返回 ErrorCode，不允许在 SDK 回调中替换。

## 线程与关闭

RtcEngine 与其 RoomEngine 共使用三个 TaskQueue；独立 RoomEngine 仅有 control 和 notification：

- control：RoomEngine 状态、Transport 请求/事件、端点收发、操作结果匹配。
- lifecycle：RtcEngine 轨道目录、GraphEngine 构图、启停和媒体资源释放。
- notification：RtcEngine / RoomEngine 的公开 Completion 与持续 Observer。RoomEngine 向
  RtcEngine 的内部结果和事件续接也经过该队列，再投递到 lifecycle；慢用户回调可能延迟
  构图或清理，不能假设内部续接完全隔离于应用通知。RoomServices 使用独立服务契约。

join、发布订阅及轨道/成员控制通过 post 入队，不等待网络 ACK。快照、媒体装配、共享输入
创建、Observer 替换及 leave 等同步接口可能等待对应队列；不能把它们一概当作非阻塞请求。
completion 不在提交调用栈内联执行，但另一线程可能在提交方法返回前执行它。
回调可以提交下一次请求，不得等待其它回调、同步退会、替换 Observer 或销毁引擎。
媒体输出回调仍使用原有输出线程。

leave/close 保持同步；只能由外部拥有者线程调用，close 仍是 leave 的别名。
先停止应用并发请求及写入，再停止图和后端、结算取消结果并清空通知队列。
leave 的可选 completion 在 notification 执行，正常返回前已交付；之后可重新 join。
回调内调用 leave 会通过 completion 返回 invalid_thread，不执行退会。

## 非控制接口与异常迁移

enable_media 和 set_observer 返回 ErrorCode；create_shared_input 与 feedback 返回 Result。
write 对未知轨道返回 not_ready，publication 的未知轨道快照包含 failed 状态及 FailureInfo。
带音频配置的 create_livekit_engine 返回 Result<unique_ptr<RtcEngine>>；也可以
先使用无参数工厂，再检查 enable_media 返回的错误码。

媒体图、设备和部分旧实现仍使用异常，由 legacy_control_boundary 转换为显式错误。
部分后端目标已单独禁用异常，见 [原生回调迁移记录](rtc-native-completion-20261031.md)；
这不代表整个 SDK 已通过禁异常构建。当前引擎没有额外的截止时间调度器或图恢复状态机，
超时和重连范围取决于后端。RtcEngine / RoomEngine 控制方法没有逐次 ControlOptions 参数。

## 调用示例

```cpp
engine.join(options, [&engine, spec](Result<RoomSnapshot> joined) {
  if (!joined) {
    report_error(joined.error());
    return;
  }
  engine.publish_media(spec, [](Result<MediaPublication> published) {
    if (!published) {
      report_error(published.error());
      return;
    }
    use_media_track(published->track);
  });
});
```

将原 on_joined/on_published/on_subscribed/on_unpublished/on_unsubscribed/on_left
移到对应调用的 completion。远端轨道发现改用 Observer 的 remote_track_added 事件。
RtcGraphApi 同样接收逐次 completion；非安装的 BlockingEngine 直接等待该回调，
不再用 Observer/TrackId 建立成功结果映射。

本次为文档源码核对，未新增运行验证；2026-10-31 的历史验证见 [回调重构记录](rtc-callback-observer-20261031.md)。以下为旧版历史记录。

## 历史验证：三线程拆分前的主动回调版本

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
