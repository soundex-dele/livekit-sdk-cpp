> 历史文档：旧 RtcEngine / RoomEngine 及底层实现已移除。本文仅记录当时设计或验证，不代表当前 API；参见 [迁移说明](legacy-engine-removal.md)。

# RTC 单次完成回调重构（2026-10-31）

控制操作现在通过每次调用的 Completion<T> 交付 Result<T>，持续状态通过 RoomObserver /
RtcObserver 通知。移除了 RtcCallbacks 以及 Observer 中的操作成功方法，内部 RtcEngine
通过 RoomEngine 的结果续接构图。control、lifecycle、notification 和同步 leave/close 保持。
公开 completion 与 Observer 均在 notification 线程，不内联执行用户回调。

## 错误和迁移

- 本地校验、后端同步/异步失败、构图失败、退会取消都有显式结果。
- 关联使用既有 BindingToken 和控制请求 ID；重复结果不再次交付。重连的 connected
  事件不重复触发最初 join completion。Observer 更换不会丢失待完成回调。
- 端点解绑完成后再交付删除结果；内部结果不经过用户通知队列。
- 非安装 BlockingEngine 直接等待每次调用的 completion，取消了按 TrackId 推断结果的表。
- 为符合禁异常规范，set_observer、enable_media 返回 ErrorCode；create_shared_input、
  feedback 和带 AudioOutputOptions 的工厂返回 Result。普通 write 使用已有状态码；
  publication 查询失败使用 TrackSnapshot 的失败状态和 FailureInfo。
- 底层存量异常通过既有 control_boundary 适配。未全仓清理异常，也未启用全目标
  -fno-exceptions；本轮没有新增超时调度器、任务线程、自动图恢复或逐请求取消接口。
- 示例、媒体 SFU 客户端、阻塞适配器和调用方已迁移。UAV 测试辅助函数补充显式返回类型，
  解决 Clang 在类内定义之前调用 auto 返回类型模板的编译错误。

## 自动化验证

最终原生构建通过：rtc backend/pipeline/architecture/media engine 测试、媒体 SFU 客户端、
automatic_session 示例和公共 engine 头文件。普通 room_client 示例及编码 SFU 客户端也已构建。

最终 CTest **5/5 通过**：rtc_backend、rtc_pipelines、rtc_dependency_boundaries、rtc_architecture、
rtc_media_engine。覆盖无 Observer 完成、通知线程身份、重复 connected、Observer 替换、
本地拒绝、后端拒绝、控制结果匹配、解绑等待、未完成操作取消、构图失败、回调内提交及
回调内同步退会拒绝。此前纯后端配置的 5 项测试也已通过。

CLI main/session、UAV 测试入口以及 conference_viewer/session_runtime 完成编译器语法检查；
这不等于桌面应用已完整链接或在真实设备上验收。旧会议示例构建缓存缺少全局 support
包含路径，单独语法检查使用现有 flags 并补上当前 CMake 中的 support 路径。

最终无窗口本地 C++ SFU 集成通过：join/publish/subscribe/leave 回调各 2 次，收到音频 31 帧、
视频 10 帧，NetEq 10ms 及退会后帧存储保留检查通过。报告：
[out/rtc-livekit-integration/20261031-220319-denw6ptn/result.json](../out/rtc-livekit-integration/20261031-220319-denw6ptn/result.json)。
该测试使用 observer-only 模式，没有验收 senderClock / avPresentation。

clang-format 20.1.8 检查本轮主要改动文件，Git diff 空白检查通过。
日志在 [out/rtc-callback-verification](../out/rtc-callback-verification/)，四个调用方最终语法检查见 callers-syntax.log。构建出现过磁盘空间不足；
清理旧 macos-x86_64 构建目录中可重建的测试/示例可执行文件后重建通过，源码、依赖缓存和
应用包保留。仍存在第三方的 nullability 和静态库链接警告。

以上均为编译、单测和无窗口合成媒体检查；未进行摄像头、麦克风、扬声器或显示设备人工验收。
