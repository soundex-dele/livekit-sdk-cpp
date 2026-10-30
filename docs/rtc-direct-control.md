> 历史记录：本文同步控制契约已被 [observer 异步控制](rtc-observers.md) 替代。当前 RtcEngine、RoomEngine 和 RtcGraphApi 的返回码仅表示受理，完成结果通过分职责 observer 通知。以下代码和验证记录描述旧版本。

# RTC 同步控制接口

`RtcEngine`、`RoomEngine` 和高级图接口 `RtcGraphApi` 直接在调用线程执行控制逻辑，不投递控制任务，也不返回或使用 promise/future。媒体节点、网络和通知仍保留各自的后台执行机制。

| 接口 | 返回值 | 失败表达 |
| --- | --- | --- |
| join / leave / close / enable_media / 静音 / 订阅需求 / 权限 / 成员更新 / 回调注册 | `ErrorCode` | 非 `ErrorCode::ok` |
| PublishedTrack / SubscribedTrack 控制、PublishPipeline / SubscribePipeline 关闭、MediaTrack::check | `ErrorCode` | 非 `ErrorCode::ok` |
| publish / graph().publish / graph().subscribe | `Result<TrackId>` | `!result`，通过 `error()` 读取完整错误 |
| RoomEngine 发布 / 订阅 / services / 工厂 / 共享输入 | `Result<智能指针类型>` | `!result`，成功值具有所有权 |
| publish_media / publish_video / publish_audio / subscribe_media | `Result<媒体句柄类型>` | `!result`，成功后读取 `result->track` |
| media_enabled / snapshot / publication / feedback / write | bool、快照、反馈或 WriteStatus | 查询缺失轨道返回默认值；写入关闭轨道返回 closed |

错误码成功值为零。不要把 ErrorCode 转为 bool 判断成功，应显式与 `ErrorCode::ok` 比较。有值接口用 `if (!result)` 判断失败；成功后用 `*result` / `result->` 访问数据。移动独占所有权时使用 `*std::move(result)`。不再提供错误码输出指针，也不以空值代替失败结果。

```cpp
using namespace livekit::sdk;
using namespace livekit::sdk::rtc;
auto created = engine::create_livekit_engine(AudioOutputOptions{});
if (!created) {
  report(created.error());
  return;
}
auto rtc = *std::move(created);
if (rtc->join(options) != ErrorCode::ok) {
  return;
}
auto publication = rtc->publish_video(spec);
if (!publication) {
  report(publication.error());
  (void)rtc->close();
  return;
}
(void)rtc->unpublish(publication->id);
(void)rtc->close();
```

会话操作锁覆盖整个控制操作，包括 ACK 等待和解绑屏障。RoomEngine 在调用线程获取控制器状态锁，直接调用适配器并处理事件直至完成；空闲时后台线程使用同一锁处理媒体、重连和事件。适配器方法串行执行，但不绑定某一个固定线程。等待协议时使用事件唤醒与截止时间，避免轮询。

回调注册及清理通过正在执行的通知计数等待旧回调结束。通知和媒体回调、构图 hook 内调用同步控制返回 invalid_thread。销毁前由应用停止并等待所有调用。close 终止会话且可重复调用；leave 后可再次 join。空工厂、返回空适配器或工厂抛出的错误由 join 报告。后端异常在引擎边界转换为错误码或失败结果，运行时详细故障也通过事件和快照提供。

所有无返回数据的同步控制接口及其内部实现统一直接返回 `ErrorCode`，不再使用空结果包装；有返回数据的公共接口及内部结果统一保留 `Result<T>`。详细运行期故障通过事件和快照获取。RoomServices 数据/RPC/流接口及播放器的异步结果保持各自的接口。仓库旧应用中的 require_result 是应用层适配器，不是 SDK 的异常接口。

Android 侧在 JNI 边界转换成 Kotlin/Java 对象，错误码使用稳定映射；见 [Android JNI 结果转换示例](android-jni-results.md)。

## 2026-10-30 有值结果接口回归

- 有值接口统一返回 `Result<T>`，移除 `ErrorCode*` 输出参数和 `return_value()`，无数据控制继续返回 `ErrorCode`。
- Release 编译通过：公开头文件、结果契约与 RTC 测试、CLI、编码/媒体/房间/数据集成客户端，以及会议、远控、投屏和直播四个桌面应用。
- 六项无窗口 CTest 全部通过：result、rtc_backend、rtc_pipelines、rtc_media_engine、rtc_architecture、rtc_dependency_boundaries。新增检查覆盖发布失败和构图失败的错误 ID、domain、code、message、backend_reason 与轨道上下文保留。
- 本次集成客户端仅编译，未重跑 SFU 网络集成；Android 仅提供接入示例，未进行 JNI 构建、Android 或实际音视频设备人工验收。

## 此前同步控制重构验证记录（2026-10-30）

- Release 编译通过：公开引擎头文件独立编译、RTC 后端/流水线/媒体/架构测试、编码/媒体/数据集成客户端、音视频示例、CLI。
- 应用编译通过：livekit_conference_demo、av_remote_host、av_screen_cast、av_live_studio。
- CTest 四项全部通过：rtc_backend、rtc_pipelines、rtc_media_engine、rtc_architecture。覆盖调用线程、空工厂、错误码、协议超时、取消、回调重入、回调故障、解绑、重连及图回收。
- 隔离 Go SFU 集成通过：livekit_rtc_engine_integration 的多图编码收发、独立关闭、leave/rejoin 与服务隔离；livekit_rtc_media_integration 的合成音视频、NetEq 10ms 输出、发送时钟、音画呈现与帧持有。
- 验证使用编译、无窗口测试和合成媒体；未进行实际摄像头、麦克风或扬声器的人工验收。

核心测试命令：

```powershell
ctest --test-dir livekit-sdk-cpp/out/rtc-media-contracts -C Release `
  -R '^rtc_(backend|pipelines|media_engine|architecture)$' --output-on-failure --timeout 60 -j 2
```

网络测试使用 `tests/rtc_livekit_integration.py`，分别传入两个集成客户端及本地 `out/uav-sfu-go/livekit-server.exe`。执行目录为 SDK 仓库，每次运行创建独立端口、临时配置并自动关闭测试服务器。
