# RTC 同步 Result 重构验证记录

更新：2026-10-30。文件名沿用方案日期；下列本机测试日志目录时间为 2026-10-29。平台：Windows x64，MSVC / C++20，Release。
依据：[重构方案](rtc-sync-result-refactor-20261029.md)。

## 实施结果

- RTC 可失败控制接口和 LiveKit 工厂同步返回 `Result<T>`；媒体发布/订阅返回已完成的 `MediaPublication` / `MediaSubscription`，不再公开 pending 句柄。
- 同一会话复用 RoomEngine 私有的非递归操作锁。调用线程执行控制与建图；内部 `*_locked` 路径避免再次加锁。join、leave、close 及发布撤销相互等待，协议超时从取得锁后开始。
- 删除 RtcEngine 控制 TaskQueue、RtcExecutor 及 RTC 的 AsyncResult/CompletionSource/then 编排。保留 Player 的 AsyncResult、RoomServices 的数据 future 和原生 SDP/统计私有 future；后者通过完成通知驱动，不扫描等待结果。
- 保留 RoomEngine 的 jthread、有限容量邮箱、deadline、epoch/incarnation 以及原生解除绑定屏障。私有 OperationRecord 通过条件变量交接唯一完成结果。
- 通知、媒体、图 factory/hook 和图终止回调在获取操作锁前拒绝控制重入。关闭先停止生产、等待媒体工作线程，再解绑传输并完成通知 fence。
- 运行期图故障立即进入端点状态和通知；失败图在下一次控制或关闭时回收，后台不等待操作锁。报告按端点合并；关闭禁用旧图报告，活动轨道数仍受上限约束。
- 会议、投屏、远控、直播、SDK 示例及 CLI 保留已有应用工作线程，改为顺序处理同步结果。`support/rtc_result.hpp` 是未安装的应用/测试辅助函数，将失败转交已有应用异常边界；它不是另一个 SDK 异步接口。SDK 内部遗留异常仍在控制/线程边界转换为结构化失败。

## 自动化验证

### 编译

SDK 构建目录 `out/rtc-media-contracts`，命令前缀：

```powershell
cmake --build out/rtc-media-contracts --config Release --parallel 6 --target <targets>
```

目标覆盖公共头文件、Result、backend/pipeline/native/media/architecture 测试，room/engine/media/data/client-extensions/region/rejoin/audio/video 集成客户端、六个常规示例、CLI、SDK 会议 demo 和 UAV 客户端。日志：`out/rtc-sync-verified-build.log`。

产品构建目录 `../livekit-app/out/rtc-products-qt6`，目标为 `livekit_conference_demo`、`av_screen_cast`、`av_remote_host`、`av_live_studio`，以及 `screen_cast_session_test`、`remote_lifecycle_test`。日志：`../livekit-app/out/rtc-sync-verified-products-build.log`。

### 单元与无窗口契约测试

```powershell
ctest --test-dir out/rtc-media-contracts -C Release -R '^(result|rtc_backend|rtc_pipelines|rtc_native|rtc_architecture|rtc_media_engine)$' --output-on-failure --timeout 45
ctest --test-dir ../livekit-app/out/rtc-products-qt6 -C Release -R '^(screen_cast_session|remote_lifecycle)$' --output-on-failure --timeout 45
```

最终结果见下方验收状态。SDK 日志 `out/rtc-sync-verified-tests.log`，产品日志 `../livekit-app/out/rtc-sync-verified-product-tests.log`。

新增/调整的关键断言：

- Result 的 void、move-only、成功/失败指针、共享错误 ID 和异常转换。
- leave 等待 join 成功、失败、超时；close 不抢占 join；排队等待不消耗控制请求协议预算。
- 通知、factory/hook、媒体输出回调的同会话/跨会话重入拒绝；解绑、旧句柄隔离和关闭后的回调屏障。
- 原生统计 future 的完成通知确实触发，避免 transport 永久保留 stats_pending。
- GraphEngine 启停、建图失败回滚、源租约释放、共享采集分支隔离、故障关联和后续控制回收。
- 产品测试覆盖重复停止、授权前取消和退出清理，不启动真实屏幕采集。

### 隔离 SFU 集成

使用本机 Go SFU `../out/uav-sfu-go/livekit-server.exe`，每个 runner 分配独立回环端口并在结束时回收进程。没有使用桌面自动化。

| 场景 | 结果与覆盖 | 日志（SDK out/ 下） |
| --- | --- | --- |
| 编码图 | 通过：多轨、独立关闭、leave/rejoin、旧服务隔离、无媒体轮询 | rtc-sync-final-network-engine.log |
| 普通音视频 | 通过：同步控制、NetEq 10 ms、发送时钟、A/V 播放时钟、保留帧 | rtc-sync-final-network-media.log |
| RoomEngine | 通过：多轨、重新订阅、顺序发布撤销、成员 ACK、静音、接收需求、订阅权限 | rtc-sync-final-network-livekit.log |
| 数据服务 | 通过：数据、RPC、分块流、多 DataTrack；使用 Agent 发布者 | rtc-sync-final-network-data-agent.log |
| 区域选择 | 通过：可信重定向、发现、取消/超时、首连接失败转移、终止错误 | rtc-sync-final-network-region.log |
| 断线重入 | 通过：两次重入、旧 epoch 拒绝、静音恢复、已移除轨道不重发、终止离会 | rtc-sync-final-network-rtc_rejoin.log |

复现普通集成使用：

```powershell
python tests/rtc_livekit_integration.py --client out/rtc-media-contracts/Release/livekit_rtc_engine_integration.exe --server ../out/uav-sfu-go/livekit-server.exe
# data 客户端需追加 --publisher-kind agent，保留服务端的转录身份策略。
```

区域及重入分别运行 `tests/region_integration.py`、`tests/rtc_rejoin_integration.py`，使用相同 `--client` / `--server` 参数。测试依赖 `websockets==15.0.1` 安装在本次忽略目录 `out/rtc-sync-tools`，以 `PYTHONPATH` 加载，没有修改全局 Python 环境。

过程中发现并处理的测试条件：普通发布者无法通过 Go SFU 的转录身份策略，改用 runner 已有的 Agent 选项后通过；C++ SFU 不支持该 runner 的 Go 配置字段，最终使用上述 Go SFU。编码图夹具只在关闭阶段排除 SFU 的精确 80 字节 Opus 静音及 31 字节 VP8 空白帧，不将其计为应用帧，其余载荷仍严格比较。

## 线程归属审查

| 对象 | 保留的归属与交接 |
| --- | --- |
| RoomEngine 协议状态 | 原有房间 jthread 独占；调用线程仅提交内部命令并等待记录，后台完成不取操作锁 |
| WebRTC PeerConnection / SDP / stats | 原有 signaling/worker/network 线程及 BlockingCall；完成通知返回已有 session loop |
| 视频、音频编解码 | 节点自己的 worker 创建/运行/清理 codec；控制线程负责启动和 join |
| 采集、外部源、FFmpeg | 原有设备适配器与输入 worker；关闭先停读者/生产者再释放租约 |
| 原生音频播放 | 现有 NativeAudioOutput 私有线程通过 BlockingCall 初始化和终止 ADM |
| GraphEngine / 输出 | 调用线程串行建图/启停；输出有界 dispatcher；UI 呈现在应用原有线程 |
| 跨房间共享输入 | 独立源图向各分支有界队列分发，不在一个会话操作锁内取得另一会话的操作锁 |

这是源码审查和对应自动化契约测试的结论，不是任意第三方插件都不存在死锁的证明。自定义节点必须遵守 quiesce 不抛异常、回调最终返回、固定线程资源由适配器交接的约定。

## 验收状态与限制

- SDK 及上述全部消费者目标编译成功，产品四个应用和两个生命周期测试目标编译成功，最终构建命令退出码均为 0。
- SDK 6/6 契约测试通过，产品 2/2 无窗口生命周期测试通过；上述六项隔离 SFU 集成通过。
- 新增 C++ 文件通过 clang-format 20.1.8 检查；已有 C++ 文件仅格式化变更范围。SDK 与 livekit-app 的 `git diff --check` 均通过。

- 只验证 Windows Release 与回环 SFU。没有进行真实摄像头、麦克风、扬声器、桌面呈现的人工设备验收，也没有验证 macOS/Linux。
- 本次没有承诺端到端调用时长等于 operation_timeout；锁等待和安全清理可能增加耗时，close 不会抢占前一个操作。
- 后台失败图需要下一次控制或关闭完成回收；长期空闲应用应依据故障通知安排关闭。
- 产品构建出现原有 WebRTC pragma / Qt 部署环境警告；具体构建结论以命令退出码为准。
- 未提交或推送代码；保留工作区已有 README 和方案内容，并同步本次实施状态。
