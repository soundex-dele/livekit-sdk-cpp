> 历史文档：旧 RtcEngine / RoomEngine 及底层实现已移除。本文仅记录当时设计或验证，不代表当前 API；参见 [迁移说明](legacy-engine-removal.md)。

# RTC 三线程精简实现（2026-10-31）

> 三线程基线记录。后续接口已迁移为[单次完成回调与持续 Observer](rtc-callback-observer-20261031.md)，线程与同步退出模型保持。

按最新要求，仅保留 control、lifecycle、notification 三个执行线程。上一轮完整加固方案已撤回，其验证结果不适用于本版。

| 线程 | 职责 |
| --- | --- |
| control | 房间状态、协议请求和事件、端点发送与接收 |
| lifecycle | 轨道目录、GraphEngine 构图、启停、释放媒体资源 |
| notification | RtcCallbacks、独立 RoomEngine 的 RoomObserver 回调 |

RtcEngine 与内部 RoomEngine 共用同一个 RtcExecution；使用现有 utoolkit TaskQueue，执行适配器只包含 post、invoke、fence。后台网络、节点、设备原有线程不属于新增三线程的范围。

已撤回额外媒体发送线程、Transport 媒体端口层、截止时间调度、准入配额与终态槽、operation_completed/operation_failed 扩展、轨道恢复状态机和旧句柄代际加强。公共接口保持原来的 void 与成功回调。

保留线程拆分所必需的同步：控制状态归 control；图和资源归 lifecycle；编码写入口使用短锁保护；外部 leave 等待线程中已排入的工作，避免释放后仍执行回调。应用先停止自己的并发调用，再退会/销毁。异步任务异常仅在工作线程边界记日志，不增加失败恢复机制。

## 本版验证

- 纯后端构建通过；rtc_backend、rtc_pipelines、rtc_dependency_boundaries **3/3 通过**。
- 原生构建通过：rtc_architecture、rtc_media_engine、公共 engine 头文件检查及 media integration。两项原生单测均通过。
- 无效共享音频配置测试按线程边界调整：等待控制/构图任务完成后，确认未生成可写轨道，已有两个共享消费者继续工作；不再等待异步构图异常同步抛回。
- 本地隔离 C++ SFU 集成通过：入会、发布、订阅、退会回调各 2 次，收到音频 34 帧、视频 10 帧，NetEq 10ms 与帧存储保留检查通过。
- clang-format 20.1.8 和 git diff --check 通过。

日志保存在本地忽略目录 [rtc-three-thread-verification](../out/rtc-three-thread-verification/)；SFU 报告见 [result.json](../out/rtc-livekit-integration/20261031-135215-918ap8yy/result.json)。此前完整加固版的 ThreadSanitizer 结果不用于本版；旧检测构建目录已因磁盘空间不足清理。

SFU 使用 observer-only 模式，未验收 senderClock / avPresentation。真实摄像头、麦克风、扬声器和显示设备人工验收不包含在此次自动化验证中。

## 线程入口简化

RoomEngine、RtcEngine 和媒体装配入口共移除 22 处当前线程判断及递归转发。同步入口将正文直接交给 invoke，由执行器统一判断是否就地执行；异步发布/订阅通知直接 post 到 lifecycle。三线程职责和原有调用行为保持不变，未增加调度机制。

本次重新编译并通过 rtc_backend、rtc_pipelines、rtc_dependency_boundaries、rtc_architecture、rtc_media_engine 共 5 项回归；格式与 diff 检查通过。本次没有重跑 SFU 集成或设备人工验收，上述 SFU 结果属于线程入口简化之前的版本。
