# ImGui 多人会议观看客户端

基于 Dear ImGui v1.91.9b 和 SDL2 的桌面客户端，支持 Windows 和 macOS 构建，界面使用英文。客户端在可滚动的视频网格中展示多位参与者，并播放远端音频。同一参与者可同时发布多路生成的测试视频，以及独立的 440 Hz 测试音调，方便打开多个实例演示会议，无需摄像头、麦克风或 FFmpeg。当前未实现自动重连、屏幕共享和聊天。

## 构建与运行

请先按照仓库根目录的说明准备 VS 2022、原生 WebRTC 和 vcpkg 依赖，然后执行：

```powershell
$env:VCPKG_ROOT = 'D:\vcpkg'
cmake --preset windows-x64 -DLIVEKIT_BUILD_DEMO=ON
cmake --build --preset windows-release --target livekit_conference_demo
.\out\windows-x64\Release\livekit_conference_demo.exe
```

作为顶层工程构建时，`LIVEKIT_BUILD_DEMO` 默认开启；通过 `add_subdirectory` 引用时默认关闭。同时关闭此选项和 `LIVEKIT_ENABLE_VIDEO_PLAYOUT`，即可构建不依赖 SDL 的 SDK。只有启用 demo 时才会下载 ImGui，其版本和 SHA-256 校验值均已固定；依赖库沿用工程的静态 MSVC 运行库配置。保留了 Linux 代码路径，但尚未验证。

## macOS 构建与打包

需要 Xcode / Command Line Tools、CMake 3.25+、NASM、Homebrew LLVM（含匹配的 libc++），以及与目标架构一致的 OpenSSL、libvpx、dav1d。使用同级仓库中包含 Apple CMake 支持的 WebRTC。以下命令在 SDK 根目录运行：

```sh
brew install cmake nasm llvm openssl@3 libvpx dav1d
# Intel 使用 macos-x86_64；Apple Silicon 使用 macos-arm64。
preset=macos-x86_64
cmake --preset "$preset" \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" \
  -DCMAKE_PREFIX_PATH="$(brew --prefix libvpx);$(brew --prefix dav1d)"
cmake --build --preset "$preset"
ctest --preset "$preset"
cmake --install "out/$preset" --prefix "$PWD/out/$preset/package" --component Conference
open "out/$preset/package/livekit_conference_demo.app"
```

构建目录中的 `Release/livekit_conference_demo.app` 依赖本机库路径；安装步骤会把非系统动态库复制到应用中、改写加载路径并进行本地 ad-hoc 签名。`package/livekit_conference_demo.app` 是可复制的本地运行产物；公开分发仍需发行者的 Developer ID 签名及公证。预设最低系统版本为 macOS 14，但所用第三方库可能要求更高版本，应以依赖库和最终产物为准。每个构建目录只包含一种架构，不可混用 Intel 与 ARM 依赖。

SDK 公共 API 使用 C++20 `std::jthread` / `std::stop_token`；Xcode 16 自带 libc++ 尚未开放这些类型。macOS 预设因此统一使用 Homebrew LLVM 的编译器与 libc++（安装打包时一并收集）。可用 `-DLIVEKIT_LLVM_ROOT=/path/to/llvm` 指定其他安装前缀；更换编译器需使用新的构建目录或清理 CMake 缓存。

如果同级 WebRTC 已有匹配源码、架构及 libc++ ABI 的 CMake Release 产物，可在配置命令中追加 `-DWEBRTC_PREBUILT_DIR="$PWD/../webrtc/out/$preset"`，复用原生库以减少重复编译。此路径还需匹配的 libjpeg（Homebrew 可用 `brew install jpeg-turbo`）；配置会检查每个静态库是否含目标架构。切回源码构建时显式传入 `-DWEBRTC_PREBUILT_DIR=`。不要混用其他 WebRTC 版本或架构的库。

macOS 窗口使用逻辑点，字体按实际 backing scale 生成，以适配 Retina 和跨屏缩放。音频播放使用 WebRTC CoreAudio 后端。当前界面仅发布合成视频和测试音调，不采集摄像头、麦克风或屏幕，因此不请求这些权限；本地网络连接用途在应用 Info.plist 中声明。HTTPS/WSS 使用 Apple 原生 TLS 和系统信任库，避免依赖 Homebrew 的 CA 文件路径；WebRTC 媒体加密仍使用 OpenSSL。

无窗口验证和程序内截图（不会打开或操作桌面）：

```sh
SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software \
  "out/$preset/Release/livekit_demo_tests" "out/$preset/conference.bmp"
# 同时生成 conference.retina.bmp，验证 2x backing buffer 的布局和纹理。
python3 tests/sdk_integration.py \
  --client "out/$preset/Release/livekit_demo_integration" \
  --server /absolute/path/to/livekit-server
```

集成测试默认使用无硬件音频设备，检查实际解码与录制。扬声器可听、Retina 实屏跨屏切换、系统本地网络授权及 Finder 启动需另行人工验收，不能由无窗口测试代替。

## 加入会议

1. 启动 LiveKit Server，参见[本机服务器说明](../../docs/cli.md)。
2. 在 **Server URL** 中填写服务器的 WebSocket 地址，在 **Room** 中填写房间名，在 **Token** 中粘贴预先签发的参与者 JWT。
3. 保持 **Play remote audio**（播放远端音频）勾选，以收听其他参与者的声音。通过 **Test video streams** 选择入会时发布的视频路数（0–8，默认 0）；**Publish test tone**（发布测试音调）是独立选项，默认关闭。
4. 点击 **Join room**（加入房间）。各路远端视频会保持原始宽高比显示，标题显示参与者名称和轨道名称；没有视频、视频静音和等待首帧时分别显示对应占位提示。本机发布的画面发送给其他客户端，不作为本地预览回显。
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

打开两个客户端实例，分别粘贴对应的 Token，房间名都填写 `demo`，服务器地址都填写 `ws://127.0.0.1:7880`。将 Alice 的 **Test video streams** 设为 2，Bob 设为 0。入会后，Bob 会看到来自 Alice 的 `Test video 1` 和 `Test video 2` 两个画面。只在其中一个实例中启用测试音调，即可验证另一个实例的音频输出。

## 入会后管理多路视频

- 左侧 **YOUR VIDEO STREAMS** 显示本机活动视频数量、各路名称和 Track SID。点击 **Add test video** 添加一路，点击某一路的 **Stop stream** 只停止该路，其他视频和音频继续发送。
- 每路使用独立轨道和递增名称。停止后再添加会使用新编号；重新加入房间后从 1 开始。各路使用相同的内置动态测试图案，默认 640×360、30 fps，通过名称和 SID 区分。
- demo 将每位参与者的测试视频限制为最多 8 路，便于控制本机编码开销；这不是 LiveKit 协议或 SDK 的路数上限。
- 发布和停止在后台会话线程执行，操作期间按钮暂时禁用，界面显示进度或错误。离会会取消待处理操作并清理所有轨道。新增轨道需要的 SDP 协商由 SDK 自动处理。

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

共用的 Python 测试脚本会启动隔离服务器，并检查实际录制的 Y4M 视频和 WAV 音频。demo 集成测试覆盖两个发布者同时发送视频、同一参与者两路初始视频及动态添加第三路的实际解码、单路停止后其他路继续出帧、全部停止后重新发布、音频轨道保留、发布过程中离会、已有成员和后加入成员、类型化轨道映射、静音与恢复、重复入会、立即取消、无效 JWT、房间不匹配、回调异常和服务端移除参与者。

`video_frames` 测试验证平面步长、旋转处理和每轨道最新帧的行为；`demo_render` 通过读回 SDL/ImGui 渲染像素检查颜色与缩放，无需摄像头或物理显示器。

如需生成用于界面检查的位图，将环境变量 `SDL_VIDEODRIVER` 设为 `dummy`、`SDL_RENDER_DRIVER` 设为 `software`，然后运行 `livekit_demo_tests.exe <output.bmp>`，将 `<output.bmp>` 替换为输出文件路径。

验证实际扬声器输出需要可用的音频设备。在运行集成测试前设置 `LIVEKIT_DEMO_TEST_AUDIO=1`，可同时验证 Windows 默认音频输出路径；仅验证录制文件不能确认物理扬声器实际可听。
