# livekit-sdk-cpp

[客户端能力补齐进度](docs/client-parity-progress.md)：已加入固定上游协议字段检查、连接恢复及首次信令 failover、TokenProvider、操作取消/确认、I420/RGBA/PCM 输入和有界帧读取，以及[订阅权限/接收质量](docs/subscription-permissions.md)、[DTMF/转写](docs/data-messages.md)、[RPC](docs/rpc.md)和[能力查询](docs/capabilities.md)。C++↔Go 单声道 PCM、官方数据消息与 RPC 已完成本机互通验证。恢复语义见[连接恢复](docs/reconnection.md)，媒体接口见[视频输入](docs/video-source.md)、[音频输入](docs/audio-source.md)、[视频读取](docs/video-stream.md)和[音频读取](docs/audio-stream.md)。编码帧/RTP、Simulcast、streams/files、DataTrack、E2EE 及跨平台验收仍待完成。

[远程控制](../docs/integration/remote-control/README.md)：新增 `RemoteControlSession`、可靠键鼠协议、租约/心跳 watchdog 和 Windows 输入后端；搭配相邻 ImGui 被控端、浏览器及 Go 授权服务使用。自动化只验证合成媒体和输入记录器，实际桌面需人工验收。

[WebRTC 媒体图节点](docs/media-graph-webrtc.md)：采集、音视频编解码与渲染拆为独立节点，四类 Factory 默认接入 Room；支持摄像头/主屏输入、可选本地预览及节点统计。视频接收通过自定义 DecoderFactory 将待解码帧送入每轨完整接收图，解码像素直接进入图内呈现、渲染与录制，不回交 WebRTC；音画同步及统计边界见该文档。

[仓库设计](docs/design.md)说明当前代码分工、线程契约及向统一媒体 SDK 演进的路径；[平台总体设计](../docs/architecture/platform-design.md)定义跨仓库边界。

新增独立的 [本地文件播放器](docs/player.md)：`LiveKit::Player` 提供 FFmpeg 库级播放、暂停、Seek 与音视频帧输出，可关闭 RTC 单独构建；桌面界面及构建脚本位于 [livekit-app](../livekit-app/apps/player/desktop/README.md)。

C++20 LiveKit SDK，底层直接链接原生 **libwebrtc**。从同级 `livekit-cli-cpp` 提取信令、媒体和管理能力，CLI 已迁入 `cli/`，仅使用 SDK 的公开头文件与 `LiveKit::SDK` CMake target。

当前版本 **0.3.0** 按同级 `server-sdk-go/v2` 适配客户端和服务端核心 API：`Room / Participant / TrackPublication`、动态音视频发布/订阅、可靠/丢包数据、参与者状态及事件，以及 `LiveKitAPI` 的 Room、Egress、Ingress、SIP、AgentDispatch、Connector 六类服务（49 个 RPC）。没有 Go 子进程或 Rust FFI。

完整方法对应关系、线程语义和范围见 [Go SDK 适配说明](docs/go-sdk-compatibility.md)。服务端请求/响应使用 protobuf JSON；媒体仍由原生 libwebrtc 处理。

## 构建与运行

已适配的依赖是同级 `../webrtc` 的 M137 CMake 工程；普通 Chromium GN checkout 不能直接替代。Windows 需要 VS 2022 C++ 工具、Windows SDK、CMake 3.25+、Git、NASM、Perl。默认静态链接 `/MT`（Debug 为 `/MTd`）。

```powershell
cd D:\workspace\livekit-sdk-cpp
$env:VCPKG_ROOT = 'D:\vcpkg'
& "$env:VCPKG_ROOT\vcpkg.exe" install openssl libvpx dav1d --triplet x64-windows-static
.\scripts\build-windows.ps1 -WebRTC D:\workspace\webrtc
.\out\windows-x64\Release\lk.exe doctor
```

首次构建下载固定版本依赖并完整编译 libwebrtc。产物：

- `out/windows-x64/Release/livekit_sdk.lib`：SDK 静态库，通过 CMake target 自动连接完整依赖。
- `out/windows-x64/Release/lk.exe`：移植后的 CLI。
- `out/windows-x64/Release/livekit_sdk_example.exe`：只调用公开 API 的示例。

完整 CLI 用法、启动本机 LiveKit Server、发布/订阅、浏览器观看见 [CLI 文档](docs/cli.md)。支持的命令和参数保持兼容；浏览器页面在 `web/`，开发服务在 `scripts/viewer-server.cjs`。

## 在 C++ 工程中使用

目前提供**源码级 CMake 集成**；不要只复制 `livekit_sdk.lib`，它仍需原生 WebRTC 等静态依赖。公开头文件仅依赖 C++20 标准库与 nlohmann/json，不泄露 WebRTC、Protobuf、SDL 或 IXWebSocket 类型。

```cmake
cmake_minimum_required(VERSION 3.24)
project(my_livekit_app LANGUAGES C CXX)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set(LIVEKIT_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LIVEKIT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIVEKIT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIVEKIT_ENABLE_VIDEO_PLAYOUT OFF CACHE BOOL "" FORCE)
set(WEBRTC_SOURCE_DIR "D:/workspace/webrtc" CACHE PATH "")
add_subdirectory("D:/workspace/livekit-sdk-cpp" livekit-sdk)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE LiveKit::SDK)
```

Windows 配置时也要传入 vcpkg toolchain 与 `x64-windows-static` triplet。`tests/consumer/` 提供完整独立工程样例。关闭 CLI 后不引入 CLI11，关闭视频窗口后不引入 SDL2。SDK 提供 macOS 构建预设；会议应用及 `.app` 打包入口已迁至 [livekit-app](../livekit-app/apps/conference/desktop/README.md#macos-构建与打包)。Linux 提供 preset，但本次未验证。

```cpp
#include <livekit/livekit.hpp>
#include <thread>
namespace sdk = livekit::sdk;

void publish(const std::string& url, const std::string& token) {
  sdk::RoomCallback callbacks;
  callbacks.on_data_received = [](const std::vector<uint8_t>& data,
                                  const sdk::DataReceiveParams& params) {
    // 处理 data / params.topic / params.sender_identity。
  };
  auto room = sdk::connect_to_room_with_token(url, token, callbacks);
  auto local = room->local_participant();
  sdk::MediaSpec video;
  video.demo = true;
  auto publication = local.publish_track(video, {"camera"}).get();
  local.publish_data({'h', 'i'}, {true, "chat"}).get();
  std::this_thread::sleep_for(std::chrono::seconds(10));
  local.unpublish_track(publication.sid()).get();
  room->disconnect();
  room->wait(); // 重新抛出后台会话错误
}
```

`join_with_token()` 在连接成功后返回，房间名可从 Token 推断。`join(url, ConnectInfo)` 在可信环境中使用 API key/secret 生成入会 Token。通过 `local_participant()`、`remote_participants()`、`participant_by_identity()`、`participant_by_sid()` 查询当前对象。公开 `info()` 返回线程安全的状态快照。

媒体支持生成图案/音调、默认麦克风、文件和 FFmpeg 网络输入；`probe_media(path)` 返回可发布的 `MediaSpec`。`RoomOptions.record` 录制接收的 WAV/Y4M。使用 SDL 视频窗口时，通过原有阻塞式 `Room::run(options, json_handler, stop_token)` 在主线程执行。

服务端管理与签发 Token：

```cpp
sdk::LiveKitAPI api({url, {api_key, api_secret, ""}});
auto room_info = api.room().create_room("demo");
auto rooms = api.room().list_rooms();
auto participants = api.room().list_participants("demo");
auto dispatch = api.agent_dispatch().create_dispatch(
    sdk::Json{{"room", "demo"}, {"agentName", "assistant"}});
auto token = api.room().create_token().set_identity("publisher")
    .set_video_grant({{"roomJoin", true}, {"room", "demo"}}).to_jwt();
```

`LiveKitAPI()` 可读取 `LIVEKIT_URL` 与凭证环境变量。只有完全未传显式凭证时才回退到环境；先取 `LIVEKIT_TOKEN`，没有 Token 再取 `LIVEKIT_API_KEY / LIVEKIT_API_SECRET`。显式 Token 优先，显式的部分 key/secret 不会与环境混用。`APIOptions.use_environment=false` 禁止环境回退；单独构造服务客户端不读取环境。API Secret 应保留在可信后端。

## 生命周期与事件

- `join*()` 使用 Room 拥有的后台线程；`run()` 在调用线程上阻塞。两种方式均将所有回调（包括视频帧）串行分发到该会话线程。`join_with_token()` 的第四个可选参数为 `std::stop_token`，可取消连接及后续会话。回调执行时不持有 SDK 状态锁。
- 动态发布、取消发布、数据发送和订阅控制返回 `std::future`。在应用线程等待 `.get()`；**不要在事件回调中等待同一 Room 尚未完成的 future**，否则会阻止处理请求。回调中可提交命令或调用 `request_stop()`。
- `publish_track()` 等待服务器确认并返回 publication；订阅/metadata/settings 的 future 只确认信令已发送，最终状态以服务端事件为准。`publish_data()` 默认 lossy，`DataPublishOptions.reliable=true` 使用可靠通道；发送成功不等于接收方已处理。
- `disconnect()` 停止并等待所拥有的后台线程。`wait()` 等待结束并抛出后台错误；`last_error()` 和 `disconnect_reason()` 可查询错误和原始协议原因码。析构也会清理后台会话。应用线程串行调用 join/disconnect/wait；`request_stop()` 与查询/提交命令可跨线程使用。
- 同一个对象不能同时运行两个会话；断开后可以再次入会。旧会话的 Participant/Publication 句柄会拒绝新操作；未完成的请求在断开时结束并报告错误。
- `run()` 由外部线程调用时，Room 必须存活到该调用结束。`request_stop()` 不等待外部调用线程。多个无窗口 Room 可在同一进程并发运行。
- 控制事件队列上限 4096，命令队列上限 1024；溢出明确报错。回调异常在清理后由 run/wait 抛出。SDK 不安装信号处理器、不向 stdout 输出事件，CLI 负责信号和 JSON 输出。
- `video_playout` 要求主线程、单进程单窗口会话；`audio_playout` 与 `microphone` 使用默认设备。WebSocket 等待可取消，已经进行中的 SDP 操作最多等待 15 秒。
- HTTP 请求默认连接超时 10 秒、传输超时 30 秒，可传 `RequestOptions` 与 stop token。`APIError` 保留 HTTP status、Twirp code、message、meta。SIP 等待接听/转接的超时至少为振铃时间加 2 秒。

原 CLI JSON 事件保留，`joined`/`participant`/`track-published` 增加完整状态字段。`data.payloadBase64` 使用 URL-safe Base64。

## 验证

```powershell
ctest --preset windows-release
$server = 'D:\Downloads\livekit_1.13.10_windows_amd64\livekit-server.exe'
python tests/integration.py --lk out/windows-x64/Release/lk.exe --server $server
python tests/stream_integration.py --lk out/windows-x64/Release/lk.exe --server $server
python tests/sdk_integration.py --client out/windows-x64/Release/livekit_sdk_integration.exe --server $server
python tests/api_contract.py --client out/windows-x64/Release/livekit_sdk_tests.exe --cli out/windows-x64/Release/lk.exe
```

端到端测试各自启动隔离服务器、使用独立端口和测试凭据，并在结束时清理子进程。SDK 端到端测试在单个 C++ 进程内发布/订阅，检查录制的真实音视频、数据消息、实例隔离、停止/复用、回调异常与重入拒绝。实际执行记录见 [VALIDATION.md](VALIDATION.md)。

## 结构与范围

| 目录 | 内容 |
| --- | --- |
| `include/livekit/` | SDK 公开 API；命名空间 `livekit::sdk` |
| `src/engines/`、`src/media_nodes/`、`src/backends/` | 原生实现与私有头文件 |
| `tools/cli/` | 仅依赖公开 SDK 的命令行程序 |
| `examples/` | SDK 直接调用示例 |
| `protocol/` | LiveKit protobuf 字段兼容子集及参考快照 |
| `cmake/` | libwebrtc 和固定版本依赖配置 |
| `tests/` | 单测、CLI/SDK 端到端测试、独立集成工程 |

本版本适配 Go SDK 的核心对象、信令和服务端方法，尚非全部功能等价移植。未实现自动重连/故障转移、RPC/数据流/转写、应用自定义 PCM/I420 帧输入、RTP sample reader、simulcast/SVC、E2EE、相机/屏幕采集、同步 MP4 封装。远端轨道通过原生接收器自动解码，可录制/播放并接收轨道状态事件。工作区另有视频帧回调与会议 Demo 的并行开发，本轮 Go 适配验收不包含该 Demo。

服务端六类客户端通过协议测试；实际部署验证目前覆盖 Room 服务，未部署 Egress/Ingress/SIP/Agent/Connector 后端。文件输入经 FFmpeg 解码后由 WebRTC 重编码；录制为每轨道 WAV/Y4M，Y4M 使用名义 30 fps。

许可证和来源声明见 [LICENSE](LICENSE) 与 [NOTICE](NOTICE)。

目录重构与独立构建边界见 [仓库结构](../docs/architecture/repository-layout.md)。`LiveKit::ServerSDK` 可在关闭 RTC、Player 后独立构建。SDK 不再提供构建产品客户端的选项；远控、传屏、视频会议与播放器分别从 `livekit-app/apps/<业务>/desktop` 构建。
