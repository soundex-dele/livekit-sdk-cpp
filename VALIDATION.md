# Go 核心 API 适配验证（0.3.0）

日期：2026-10-23（Asia/Shanghai）。工程：`D:\workspace\livekit-sdk-cpp`。Go 参考版本：`743ab2d6b12ff24c2b2f9a0cc208c7afe8602571`。

## 构建

Windows x64、Visual Studio 2022 / MSVC 19.44、CMake 4.1、Release `/MT`。直接编译链接同级 libwebrtc M137（checkout HEAD `359590b40a6363b8231273d03355af54950fc339`），使用 `D:\vcpkg` 的静态 OpenSSL/libvpx/dav1d。公开 API 不暴露 WebRTC/Protobuf/SDL/IXWebSocket 类型。

`cmake --build --preset windows-release` 的 SDK、CLI、示例、测试构建通过，日志 `out-build-go-release.log`。外部 WebRTC 有既有类型转换/pragma 警告。修正测试时序时，共享构建目录发生并行对象文件写入冲突（`out-build-go-core-test.log` 的 Permission denied）。随后仅编译/链接该测试到独立输出目录，日志 `out-build-go-core-isolated.log`，成功复用已有 SDK 和依赖库。

工作区同期在开发会议 Demo/视频帧回调。其初次 CMake 配置时入口文件尚未齐全，已保留代码，并通过 `LIVEKIT_BUILD_DEMO=OFF` 完成本轮 SDK/CLI 构建。后续共享构建树又注册了新增测试；全量 CTest 一度因 Demo/video_frames 测试程序尚不存在而报告 Not Run。本记录将 Go 核心适配的五项测试单独列出，**不把并行 Demo 计入本轮验收**。

## SDK/CLI 基础测试

命令：

```powershell
ctest --preset windows-release -R '^(core|sdk|cli_help|native_webrtc|video_playout)$'
```

最终结果：**5/5 通过**，日志 `out-ctest-go-core-final.log`。为避免共享 CTest 注册表和并行输出继续变化，最终将这五项测试放入 `out/go-core-check` 独立运行目录：core 使用修正后重新编译的程序，其余使用已构建程序的副本。最终执行命令为 `ctest --test-dir out/go-core-check --output-on-failure`。涵盖 Token/URL、protobuf wire、无 shell 子进程、真实 PCM、原生 PeerConnection/SDP、回调线程和异常、预取消、会话复用、SDK 输出隔离、CLI 和 SDL 渲染。

复核过程中发现旧 PCM 测试以固定 90 ms 等待后台线程，繁忙编译时偶发不足三帧。测试改为在 3 秒期限内等待实际帧数后再停止，仍检查至少三帧、正确格式与非零 PCM。

## 真实 LiveKit Server 联调

本机 LiveKit Server **1.13.10**。各脚本启动隔离服务端、独立端口和测试凭证，结束时清理子进程。

| 测试 | 结果路径 | 验证内容 |
| --- | --- | --- |
| `tests/sdk_integration.py` | `out/sdk-integration/20261023-124050/result.json` | Go 风格后台入会、初始 roster、identity/SID 查询、动态发布/取消/再次发布、订阅切换、远端设置、静音、定向二进制 reliable 与 lossy 数据、metadata/name/属性删除、Room metadata、服务端 SendData、断开取消、旧句柄失效、踢人原因、只订阅权限 |
| `tests/integration.py` | `out/integration/20261023-124050/result.json` | JWT 独立验签、Room CRUD、错误输入、原生 VP8/Opus、数据、FFmpeg 文件和中文路径、四轨并发、实际 WAV/Y4M、EOF 清理 |
| `tests/stream_integration.py` | `out/stream-integration/20261023-124050/result.json` | TCP H.264 输入、指定轨道订阅、服务端静音/恢复、踢人后的 CLI 错误退出 |

以上三项均通过。SDK 联调摘要：

```json
{"ok":true,"goCore":true,"tracks":2,"dataMessages":1,"reusedSessions":2,
 "dynamicPublished":3,"dynamicSubscribed":4,"dynamicUnpublished":3,
 "typedDataMessages":3,"recordingsVerified":true}
```

SDK 测试只调用公开 API，在单个 C++ 进程内运行多个 Room，不经 CLI。录制检查真实视频帧数和非静音解码 PCM。

## 六类服务端 API 协议测试

`tests/api_contract.py`：**通过**。结果：`out/api-contract/20261023-124159/result.json`。

- 49 个 RPC：Room 13、Egress 10、Ingress 4、SIP 14、AgentDispatch 3、Connector 5。
- 独立 Python HMAC 校验 JWT 签名、方法对应 grant、identity/name/metadata/attributes/kind/TTL。
- HTTP 路径、protobuf JSON、UUID v4 request ID、SendData 16 字节 nonce。
- 环境 Token、显式 Token、显式 key/secret 的优先级，不完整显式凭证拒绝。
- Twirp status/code/message/meta，预取消和执行中取消。
- SIP 等待接听/转接默认振铃参数；GetDispatch 未找到返回 null。
- CLI 显式 key/secret 不被环境 Token 覆盖。

该测试使用可控 HTTP 服务，不代表已实际部署 Egress/Ingress/SIP/Agent/Connector 后端；真实服务端验证范围是 Room API。

## 边界

未验证 Linux、Debug、远程 LiveKit Cloud、TURN-only 网络、真实麦克风/扬声器、非 VP8 发布、RTSP/H.265，以及同期新增会议 Demo。本轮未修改参考 CLI、Go SDK 或外部 WebRTC 源码。自动重连/故障转移、RPC/数据流、RTP sample、simulcast/SVC、E2EE 等未实现能力见 [适配说明](docs/go-sdk-compatibility.md)。

初次 SDK/CLI 提取的历史验证保存在 [v0.2 基线](docs/validation-v0.2.md)。


## ImGui 会议 Demo（2026-10-23）

使用当前 SDK 的 `Room::join_with_token`、`RoomCallback`、`LocalParticipant::publish_track` 和类型化成员/轨道快照；不在 demo 中解析旧版 JSON 事件。新增自有 I420 视频帧回调，并为 Token 入会接口增加可选停止令牌。

- **Windows Release 产物通过**：`out/windows-x64/Release/livekit_conference_demo.exe`。ImGui v1.91.9b 下载包使用 SHA-256 固定；SDL2 与现有视频窗口共用依赖。
- **CTest 7/7 通过**：`out-demo-ctest-final.log`。包括原 core/sdk/CLI/native/video_playout 测试，以及新增 video_frames、demo_render。
- **帧测试通过**：带填充步长、奇数尺寸、90/180/270 度旋转的角点像素与尺寸、最新帧覆盖、分辨率变化、退订后迟到帧、重新注册清空缓存、回调线程和回调异常。
- **ImGui 渲染通过**：软件 SDL 后端读回验证 I420 红色没有交换色度平面，确认视频进入真实 ImGui draw data；验证比例、缩放、静音占位、无视频成员及成员离开清理。界面图：`out/demo-grid.png`（纯红画面为颜色校验测试源）。
- **多人集成通过**：`out/sdk-integration/20261023-130857/result.json`。两个发布者同时发送视频，检查初始成员、后加入成员、SID 归属、静音/恢复、取消发布、离开、测试视频动态发布、重复入会、三次立即取消、错误 Token、房间不匹配、回调抛异常及服务端移除后的 Error 状态。
- **音频路径通过**：本次多人集成设置 `LIVEKIT_DEMO_TEST_AUDIO=1`，启用 Windows 默认音频输出；Python runner 验证接收的 Y4M 视频和非静音 WAV 音频。未通过人耳确认物理扬声器听感。
- **原 SDK 端到端回归通过**：`out/sdk-integration/20261023-130848/result.json`，包括动态发布/取消发布、订阅、数据、状态、重复连接及真实音视频录制。
- **独立 consumer 完整构建通过**：`out-demo-consumer-configure.log`、`out-demo-consumer-build.log`。`LIVEKIT_BUILD_DEMO=OFF`、`LIVEKIT_ENABLE_VIDEO_PLAYOUT=OFF`，完整编译原生依赖与 SDK 并链接 `out/consumer/bin/Release/consumer.exe`；启动后正确显示用法，生成 target 中没有 SDL2/ImGui 依赖。原生 WebRTC 的现有编译警告保留。

构建过程中一次增量 AOM 链接遇到缺失中间对象，重新串行构建成功。并行完整依赖编译期间，原 core 短时音频检查和 SDK 录制帧数检查出现过失败；负载下降后串行回归全部通过，未降低检查标准。

实际 Windows 窗口已启动，但桌面处于锁屏状态，computer-use 按其锁屏规则停止操作。因此实际鼠标/键盘入会、窗口拖拽和多屏 DPI 切换验收仍待解锁后进行；离屏渲染和会话集成测试已完成。Linux、Debug、远程 Cloud/TURN 网络本轮未验证。


## 视频直接进入媒体图（2026-10-26）

自定义 DecoderFactory 接收 WebRTC 调度后的编码帧，实际解码在图节点执行，像素不再回交 WebRTC。每个接收轨道由同一 GraphEngine 管理解码、呈现、渲染和可选录制；WebRTC 源码没有改动。

- Windows x64 Release 构建通过：`out/direct-receive-build-final.log`。
- 当前配置 CTest 9/9 通过：`out/direct-receive-ctest.log`。覆盖真实 VP8、禁止像素回交、SSRC 绑定、异步回调/旧代次、队列上限、停止屏障及无窗口渲染。
- 本地 SFU 集成通过：[result.json](out/direct-receive-integration/20261026-182723/result.json)。包含 VP8/Opus 实际录制、数据、动态发布订阅；无窗口会议会话验证双发布者、单用户三路视频、增删轨道、Room 复用、应用帧回调异常。执行日志：`out/direct-receive-integration-final.log`。
- 全程未使用桌面自动化，会议会话测试显式关闭物理音频输出。物理设备、硬件 codec、弱网、长期音画同步及实际设备人工验收未执行。

此模式不再更新 WebRTC 的解码完成反馈，其 framesDecoded/解码耗时及部分丢帧统计不能代表图输出。呈现按本地单调时钟与 render_time_ms 执行，实际音频播放主时钟尚未接入；不能宣称保留了原生完整 A-V 同步。具体边界见 [实现说明](docs/media-graph-webrtc.md)。
