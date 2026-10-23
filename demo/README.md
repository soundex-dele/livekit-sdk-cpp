# ImGui 多人会议观看客户端

基于 Dear ImGui v1.91.9b 和 SDL2 的桌面客户端，优先支持 Windows，界面使用英文。客户端在可滚动的视频网格中展示多位参与者，并播放远端音频。可选发布生成的测试画面和 440 Hz 测试音调，方便打开多个实例演示会议，无需摄像头、麦克风或 FFmpeg。当前未实现自动重连、屏幕共享和聊天。

## 构建与运行

请先按照仓库根目录的说明准备 VS 2022、原生 WebRTC 和 vcpkg 依赖，然后执行：

```powershell
$env:VCPKG_ROOT = 'D:\vcpkg'
cmake --preset windows-x64 -DLIVEKIT_BUILD_DEMO=ON
cmake --build --preset windows-release --target livekit_conference_demo
.\out\windows-x64\Release\livekit_conference_demo.exe
```

作为顶层工程构建时，`LIVEKIT_BUILD_DEMO` 默认开启；通过 `add_subdirectory` 引用时默认关闭。同时关闭此选项和 `LIVEKIT_ENABLE_VIDEO_PLAYOUT`，即可构建不依赖 SDL 的 SDK。只有启用 demo 时才会下载 ImGui，其版本和 SHA-256 校验值均已固定；依赖库沿用工程的静态 MSVC 运行库配置。保留了 Linux 代码路径，但尚未验证。

## 加入会议

1. 启动 LiveKit Server，参见[本机服务器说明](../docs/cli.md)。
2. 在 **Server URL** 中填写服务器的 WebSocket 地址，在 **Room** 中填写房间名，在 **Token** 中粘贴预先签发的参与者 JWT。
3. 保持 **Play remote audio**（播放远端音频）勾选，以收听其他参与者的声音。可勾选 **Publish test video**（发布测试画面）；**Publish test tone**（发布测试音调）是独立选项，默认关闭。
4. 点击 **Join room**（加入房间）。各路远端视频会保持原始宽高比显示；没有视频、视频静音和等待首帧时分别显示对应占位提示。本机发布的画面发送给其他客户端，不作为本地预览回显。
5. 连接过程中可点击 **Cancel**（取消），入会后可点击 **Leave**（离会）。关闭窗口会请求停止会话，界面继续响应，直到资源清理完毕。网络或服务器故障会显示错误，可再次点击 **Join room** 手动重新连接。

Token 仅保留在进程内存中，不保存到文件或写入日志；客户端不提供 API Secret 输入入口。界面偏好和 ImGui 布局也不会持久化。多个实例必须使用不同的参与者身份，否则 LiveKit 会断开先前以相同身份连接的实例。

使用仓库提供的本机开发服务器时，可在终端生成两个 Token：

```powershell
$env:LIVEKIT_URL = 'http://127.0.0.1:7880'
$env:LIVEKIT_API_KEY = 'devkey'
$env:LIVEKIT_API_SECRET = 'local-dev-secret-change-me-32-chars'
$lk = '.\out\windows-x64\Release\lk.exe'
& $lk token create --join --room demo --identity alice
& $lk token create --join --room demo --identity bob
```

打开两个客户端实例，分别粘贴对应的 Token，房间名都填写 `demo`，服务器地址都填写 `ws://127.0.0.1:7880`，并在两个实例中都启用测试画面。入会后，两者即可互相观看。只在其中一个实例中启用测试音调，即可验证另一个实例的音频输出。

## 架构与公开 API

- `Session` 使用当前 SDK 的 `Room::join_with_token`、`RoomCallback`、`local_participant().publish_track()` 和类型化的 `ParticipantInfo` 快照。demo 的会话管理线程等待入会和发布操作返回的 future，SDK 自己管理原生会话工作线程，因此不会阻塞 UI 或媒体回调。
- `RoomOptions.video_frame_handler` 与其他回调在 SDK 会话线程上串行执行。`VideoFrame` 自行持有已校正旋转方向的 I420 三平面数据、各平面的行步长、画面尺寸、轨道 SID 和微秒时间戳。若需在回调返回后继续使用，应复制该对象。回调抛出异常会终止会话，并由 `run` 或 `wait` 重新抛出。
- 原生接收端为每条活动轨道最多保留一帧最新画面。只有注册视频回调后，才对实际交付的帧执行格式转换和像素复制。demo 同样只保留每条轨道的最新帧。SDK 公开头文件不暴露 WebRTC、SDL 或 ImGui 类型。
- I420 纹理上传和 SDL/ImGui 渲染均由主线程执行。移除轨道、参与者离开或断开连接时会清理纹理；DPI 改变时会调整界面缩放。硬件加速不可用时回退到软件渲染。
- 再次入会前会等待上一次会话清理完毕，并重置全部界面状态。`join_with_token` 的第四个可选参数为停止令牌，可取消连接过程和运行中的会话，也支持启动后立即取消。

## 测试与验证

```powershell
cmake --build --preset windows-release
ctest --preset windows-release
python tests/sdk_integration.py --client out/windows-x64/Release/livekit_demo_integration.exe --server D:\Downloads\livekit_1.13.10_windows_amd64\livekit-server.exe
```

共用的 Python 测试脚本会启动隔离服务器，并检查实际录制的 Y4M 视频和 WAV 音频。demo 集成测试覆盖两个发布者同时发送视频、已有成员和后加入成员、类型化轨道映射、静音与恢复、取消发布与离会、测试画面发布、重复入会、立即取消、无效 JWT、房间不匹配、回调异常和服务端移除参与者。

`video_frames` 测试验证平面步长、旋转处理和每轨道最新帧的行为；`demo_render` 通过读回 SDL/ImGui 渲染像素检查颜色与缩放，无需摄像头或物理显示器。

如需生成用于界面检查的位图，将环境变量 `SDL_VIDEODRIVER` 设为 `dummy`、`SDL_RENDER_DRIVER` 设为 `software`，然后运行 `livekit_demo_tests.exe <output.bmp>`，将 `<output.bmp>` 替换为输出文件路径。

验证实际扬声器输出需要可用的音频设备。在运行集成测试前设置 `LIVEKIT_DEMO_TEST_AUDIO=1`，可同时验证 Windows 默认音频输出路径；仅验证录制文件不能确认物理扬声器实际可听。
