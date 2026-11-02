# 同步编码帧 Room 示例

链接 `LiveKit::Client`，包含 `livekit/client/room.hpp`。设置 `LIVEKIT_URL` 和
`LIVEKIT_TOKEN` 后运行 `livekit_basic_room_encoded`，发布五秒已经编码的 Opus 静音帧。
示例不打开麦克风，不编码 PCM，不依赖 GraphEngine。

`connect()` 返回 bool；发布返回 `Result<LocalTrackPublication>`；无返回值控制记录
日志；Observer 只接收持续状态事件。不要在 Observer 中调用阻塞控制或销毁 Room。
复制的日志宏通过独立 utoolkit_logging 后端输出，首次构造 Room 时初始化一次。

要接入自己的编码器，将示例的 Opus 数据替换为编码访问单元，填写正确的 codec、
时钟频率、时间戳、时长和当前 epoch。视频还需维护关键帧及编码配置，并处理
`feedback()` 的目标码率和关键帧请求。检查 `try_write()` 的背压/未就绪状态。

`LIVEKIT_BUILD_CLIENT=ON` 构建该接口；实际 LiveKit 连接需要
`LIVEKIT_BUILD_RTC_NATIVE=ON`。关闭 native 时仍可构建注入内部 Fake RtcSession的单元测试，
默认 Room 的 connect 会返回 false。纯客户端不需要启用 RTC 图节点或媒体模块。
