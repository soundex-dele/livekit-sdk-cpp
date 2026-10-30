# RTC 同步控制接口

`RtcEngine`、`RoomEngine` 和高级图接口 `RtcGraphApi` 直接在调用线程执行控制逻辑，不投递控制任务，也不返回或使用 promise/future。媒体节点、网络和通知仍保留各自的后台执行机制。

| 接口 | 返回值 | 失败表达 |
| --- | --- | --- |
| join / leave / close / enable_media / 静音 / 订阅需求 / 权限 / 成员更新 / 回调注册 | `ErrorCode` | 非 `ErrorCode::ok` |
| PublishedTrack / SubscribedTrack 控制、PublishPipeline / SubscribePipeline 关闭、MediaTrack::check | `ErrorCode` | 非 `ErrorCode::ok` |
| publish / graph().publish / graph().subscribe | `TrackId` | 空字符串，可选 `ErrorCode*` |
| RoomEngine 发布 / 订阅 / services / 工厂 / 共享输入 | 智能指针 | nullptr，可选 `ErrorCode*` |
| publish_media / publish_video / publish_audio / subscribe_media | 媒体句柄 | track 为空，可选 `ErrorCode*` |
| media_enabled / snapshot / publication / feedback / write | bool、快照、反馈或 WriteStatus | 查询缺失轨道返回默认值；写入关闭轨道返回 closed |

错误码成功值为零。不要把 ErrorCode 转为 bool 判断成功，应显式与 `ErrorCode::ok` 比较。值接口无论成功还是失败都会写入提供的错误码参数。

```cpp
using namespace livekit::sdk;
using namespace livekit::sdk::rtc;
auto rtc = engine::create_livekit_engine(AudioOutputOptions{});
if (!rtc) {
  return;
}
if (rtc->join(options) != ErrorCode::ok) {
  return;
}
ErrorCode error;
auto publication = rtc->publish_video(spec, &error);
if (!publication.track) {
  report(error);
  (void)rtc->close();
  return;
}
(void)rtc->unpublish(publication.id);
(void)rtc->close();
```

会话操作锁覆盖整个控制操作，包括 ACK 等待和解绑屏障。RoomEngine 在调用线程获取控制器状态锁，直接调用适配器并处理事件直至完成；空闲时后台线程使用同一锁处理媒体、重连和事件。适配器方法串行执行，但不绑定某一个固定线程。等待协议时使用事件唤醒与截止时间，避免轮询。

回调注册及清理通过正在执行的通知计数等待旧回调结束。通知和媒体回调、构图 hook 内调用同步控制返回 invalid_thread。销毁前由应用停止并等待所有调用。close 终止会话且可重复调用；leave 后可再次 join。空工厂、返回空适配器或工厂抛出的错误由 join 报告。后端异常在引擎边界转换为错误码，运行时详细故障仍通过事件和快照提供。

所有无返回数据的同步控制接口及其内部实现统一直接返回 `ErrorCode`，不再使用空结果包装；有返回数据的内部结果仍保留 `Result<T>`。详细运行期故障通过事件和快照获取。RoomServices 数据/RPC/流接口及播放器的异步结果保持各自的接口。仓库旧应用中的 require_result 是应用层适配器，不是 SDK 的异常接口。

## 2026-10-30 验证

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
