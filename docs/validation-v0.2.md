# 迁移验证记录

日期：2026-10-23（Asia/Shanghai）。以下结果来自新目录 `D:\workspace\livekit-sdk-cpp`，不是沿用参考 CLI 的历史测试结果。

## 构建环境

- Windows x64、Visual Studio 2022 / MSVC 19.44、CMake 4.1、Release、静态 CRT。
- 原生 WebRTC：`D:\workspace\webrtc`，HEAD `359590b40a6363b8231273d03355af54950fc339`。
- OpenSSL、libvpx、dav1d：`D:\vcpkg\installed\x64-windows-static`。
- 首次编译了新构建目录内的 libwebrtc、SDK、CLI、示例与测试程序。
- 为避免重复下载，配置时通过标准 `FETCHCONTENT_SOURCE_DIR_*` 参数复用了参考工程的依赖源码缓存；没有使用其 `lk_core` 或旧 CLI 实现作为 SDK 运行时依赖。默认 CMake 仍可自行下载固定版本依赖。
- 构建日志：`out-build.log`、`out-build-final.log`。外部 WebRTC 仍存在类型转换等编译警告；没有编译/链接错误。

产物与 SHA-256：

| 文件 | SHA-256 |
| --- | --- |
| `out/windows-x64/Release/lk.exe` | `052567B6876C74B1C67746015D82009F46D46B2D200353C9445EE9272DEF2464` |
| `out/windows-x64/Release/livekit_sdk.lib` | `F7D93800EC64B08E95866BCF6D91EDB991620268D96A80FCF13AE5F59FFE62B2` |

`lk --version` 输出 `lk-cpp 0.2.0 (livekit-sdk-cpp)`。`dumpbin /dependents` 只列出 Windows 系统 DLL，没有独立 WebRTC/SDL DLL、Go/Rust 运行时或动态 MSVC CRT。

## CTest：5/5 通过

执行 `ctest --preset windows-release`，日志 `out-ctest.log`。

| 测试 | 覆盖内容 |
| --- | --- |
| core | Token/URL、protobuf wire 字段、无 shell 子进程参数、实际 PCM 音频源 |
| sdk | 独立事件接收器、回调线程隔离、回调异常、参数校验、预取消、原生运行时重复创建、SDK 不向 stdout 写事件 |
| cli_help | CLI 公开入口 |
| native_webrtc | 真实 PeerConnection、数据通道 SDP 创建与设置 |
| video_playout | YUV 像素读回、最新帧、分辨率/旋转、缩放、取消订阅、窗口关闭、并发 SDL 会话拒绝 |

## 最终 CLI 的 LiveKit 端到端测试

服务端：本机 LiveKit Server 1.13.10。以下测试均使用最终 `out/windows-x64/Release/lk.exe`，各自启动独立服务器并在结束时清理测试子进程。

`tests/integration.py`：**通过**。结果：`out/integration/20261023-113714/result.json`。

- 独立 Python HMAC 验证 SDK 生成的 JWT。
- 创建、查询、修改、删除房间和参与者查询；拒绝错误 Token 与奇数视频尺寸。
- 原生 VP8 视频、Opus 音频、可靠数据通道发送与接收。
- Y4M 包含实际视频帧，WAV 包含实际解码的非静音 PCM。
- FFmpeg 输入、空格/中文路径、四轨并发发布与订阅、有限输入 EOF 清理。

`tests/stream_integration.py`：**通过**。结果：`out/stream-integration/20261023-113714/result.json`。

- TCP H.264 Annex-B 输入解码后通过 WebRTC 发布。
- 仅订阅指定视频轨道，不混入音频。
- 服务端静音/恢复与移除参与者后的错误退出和清理。

## SDK 直接调用：通过

`tests/sdk_integration.py` 使用只包含公开头文件的 C++ 测试程序，未调用 CLI。结果：`out/sdk-integration/20261023-113623/result.json`。

```json
{
  "dataMessages": 1,
  "ok": true,
  "reusedSessions": 2,
  "tracks": 2,
  "recordingsVerified": true
}
```

同一进程内两个 `Room` 实例分别发布/订阅两条音视频轨道，接收数据消息并验证录制。还验证了回调在调用线程执行、运行中拒绝同对象重入、从回调/另一线程停止、停止后连续复用，以及回调抛异常后的资源清理。

## SDK 接入边界

- `livekit_sdk_example` 完成编译与链接，仅链接 `LiveKit::SDK`。
- 检查 CLI 与示例编译参数：公开依赖只有 SDK、nlohmann/json（CLI 另有 CLI11）；没有 WebRTC、生成 protobuf、SDL 或内部实现头文件路径。
- `tests/consumer/` 作为独立 CMake 工程配置通过，关闭 CLI、测试、示例和 SDL，调用程序的 C++ 编译通过（0 警告、0 错误）。日志为 `out-consumer-configure.log`、`out-consumer-compile.log`。此独立构建树未重复完整编译/链接原生依赖；上面的 SDK 示例与运行测试使用默认完整构建。

## 本轮未验证

Linux、Debug、远程 LiveKit Cloud、TURN-only 网络、真实麦克风/扬声器、RTSP/H.265 输入、非 VP8 发布和浏览器观看页。本轮未修改参考 CLI、Go SDK 或外部 WebRTC 源码。API 范围与尚未实现的功能见 README。
