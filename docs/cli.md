# SDK-based LiveKit CLI

原生 C++20 LiveKit CLI，当前迁移范围是**房间管理、音视频流发布与订阅**。
直接编译链接同级目录的 WebRTC（本机为 `D:\workspace\webrtc`），不调用 Go CLI，也不通过 Rust FFI 转接媒体。

## 构建

Windows x64 是首个支持平台，需要 Visual Studio 2022 的 C++ 工具、Windows SDK、CMake 3.25+、Git、NASM、Perl。
WebRTC 当前 CMake 依赖的系统库通过 vcpkg 安装：

```powershell
$env:VCPKG_ROOT = 'D:\vcpkg'
& "$env:VCPKG_ROOT\vcpkg.exe" install openssl libvpx dav1d --triplet x64-windows-static
.\scripts\build-windows.ps1 -WebRTC D:\workspace\webrtc
```

构建脚本会查找 vcpkg 下载的 NASM 和 Git for Windows 附带的 Perl。
也可把它们加入 PATH 后运行：

```powershell
cmake --preset windows-x64
cmake --build --preset windows-release
ctest --preset windows-release
```

输出：`out/windows-x64/Release/lk.exe`。首次构建会下载固定版本的 IXWebSocket、CLI11、nlohmann/json、Protobuf 和 SDL2，并完整构建 WebRTC。
使用静态 MSVC CRT；Debug/Release 不混用。`WEBRTC_SOURCE_DIR` 可用 CMake 参数覆盖。

Linux 使用同一套业务、信令和媒体代码，子进程层采用 `posix_spawnp`，提供 `linux-x64`/`linux-release` preset。
需为本地 WebRTC 配好 OpenSSL、libvpx、dav1d 的 CMake 包（含 `unofficial-libvpx`），以及 NASM、Perl、Ninja。
Linux 尚未在本机实测；macOS/ARM64 受当前外部 WebRTC CMake 的平台限制，暂未支持。

## 启动 LiveKit Server（Windows 本机）

房间加入和音视频收发需要一个运行中的 LiveKit Server。两个 CLI 通过服务器转发媒体；当前没有直接点对点互连模式。
`doctor`、生成 Token 不需要连接服务器。

本机已有服务器程序：`D:\Downloads\livekit_1.13.10_windows_amd64\livekit-server.exe`。
其他机器可从 [LiveKit Server Releases](https://github.com/livekit/livekit/releases) 下载 Windows amd64 压缩包并解压，按实际位置修改下面的 `$server`。

在第一个 PowerShell 窗口启动服务器：

```powershell
Set-Location D:\workspace\livekit-sdk-cpp
$server = 'D:\Downloads\livekit_1.13.10_windows_amd64\livekit-server.exe'
& $server --version
& $server --config .\config\livekit-local.yaml
```

仓库提供了可直接使用的 [本机配置](../config/livekit-local.yaml)，端口和凭据如下：

| 配置 | 值 |
| --- | --- |
| HTTP API / WebSocket 信令 | `127.0.0.1:7880`（TCP） |
| WebRTC TCP | 本机网卡地址的 `7881/TCP` |
| WebRTC UDP | 本机网卡地址的 `7882/UDP` |
| API Key | `devkey` |
| API Secret | `local-dev-secret-change-me-32-chars` |

看到 `starting LiveKit server` 日志后保持该窗口运行。服务器在前台运行，按 **Ctrl+C** 停止；停止后房间内的 CLI 会断开连接。
如果启动时报端口被占用，可修改配置中的端口；修改 `port` 后也要相应修改 CLI 的 `LIVEKIT_URL`。
上述单机配置不需要 Redis 或 LiveKit Cloud 账号，示例凭据仅用于本机开发。

HTTP/信令只绑定回环地址，媒体端口会监听本机网卡。不要将 `rtc.node_ip` 固定为 `127.0.0.1`：
这会改写 ICE 媒体候选地址，可能导致同机浏览器报 `could not establish pc connection`。
省略 `node_ip`、保留 `use_external_ip: false`，让服务器公告本机网卡地址；修改后需要重启服务器和发布端。

需要让局域网内其他机器连接时，将配置中的 `bind_addresses` 和 `rtc.node_ip` 改成服务器的实际局域网 IP，例如 `192.168.1.10`，
客户端使用 `http://192.168.1.10:7880`，并确保服务器防火墙允许上述两个 TCP 端口和 UDP 端口。
局域网部署应替换示例 Key/Secret，并同步更新客户端。公网部署还需要按 LiveKit 的部署要求配置 TLS 和网络可达性。

此前端到端测试由脚本临时启动独立服务器，使用随机端口，结束后自动关闭；测试不会留下常驻服务。手动使用 CLI 时需按本节启动服务器。

## 连接设置

在第二个 PowerShell 窗口配置 CLI。每个新的 CLI 窗口都需要执行这段设置：

```powershell
Set-Location D:\workspace\livekit-sdk-cpp
$env:LIVEKIT_URL = 'http://127.0.0.1:7880'
$env:LIVEKIT_API_KEY = 'devkey'
$env:LIVEKIT_API_SECRET = 'local-dev-secret-change-me-32-chars'
# 清除之前的 Token，使用与服务器配置匹配的 Key/Secret
Remove-Item Env:LIVEKIT_TOKEN -ErrorAction SilentlyContinue
$lk = '.\out\windows-x64\Release\lk.exe'
& $lk doctor
& $lk room list
& $lk token create --join --room demo --identity publisher
```

`room list` 成功输出 JSON 即表示服务器 API 与鉴权可用，空房间列表也是正常结果。
接下来在这个窗口运行下文的订阅命令，再打开第三个窗口执行相同连接设置和发布命令。

也支持 `--url`、`--api-key`、`--api-secret` 和 `--token`，参数可放在子命令后。
预签发 JWT 可通过 `LIVEKIT_TOKEN` 传入；它必须包含对应操作的授权。使用 Token 加入时，位置参数的房间必须与 Token 相符。
HTTP/WSS 使用证书和主机名校验；Bearer Token 放在请求头中。

## CLI 发布，浏览器观看（同一台电脑）

先按上文启动 LiveKit Server。观看页仅订阅，不请求摄像头或麦克风，也不依赖 `meet.livekit.io`。
需要 Node.js 18+；首次安装 SDK 需要联网，之后页面和 SDK 都从本机加载。

```powershell
Set-Location D:\workspace\livekit-sdk-cpp
npm.cmd ci --prefix web --ignore-scripts --no-audit --no-fund
node .\scripts\viewer-server.cjs --room test
```

打开 <http://127.0.0.1:8080>，点击“连接观看”。如果声音没有自动播放，点击“启用声音”。
另一个配置好连接环境变量的终端运行：

```powershell
& $lk room publish test --identity publisher --publish-demo --publish-tone --codec vp8
# 或循环播放本地文件（需要 FFmpeg）
& $lk room publish test --identity publisher --publish 'D:\videos\sample.mp4' --loop --codec vp8
```

观看页显示订阅轨道、接收字节、视频解码帧数和选中的 ICE 媒体路径。
`--room` 决定 Token 的房间，须与发布端一致；每次连接生成不同身份的仅订阅 Token，有效期一小时。
API Secret 保留在本地 Node 进程中，不发送给网页；Token 不写入 URL 或磁盘。
服务默认使用本文的本地开发凭据，可通过 `LIVEKIT_API_KEY`、`LIVEKIT_API_SECRET` 覆盖。
`--url` 指定服务器信令地址、`--port` 指定观看页端口，例如：

```powershell
node .\scripts\viewer-server.cjs --room test --url http://127.0.0.1:7880 --port 8080
```

观看页服务只监听 `127.0.0.1`，仅供本机调试；在其终端按 Ctrl+C 停止。
如果需要修改 CLI 可执行文件位置，设置 `LIVEKIT_CLI`。

如果出现 `ConnectionError: could not establish pc connection`：

- 使用新版 `config/livekit-local.yaml`，删除旧的 `rtc.node_ip: 127.0.0.1` 并重启服务器和发布端。
- 确认防火墙允许 LiveKit 媒体端口。信令能连接不代表媒体路径可达。
- 浏览器与服务器不在同机时，应部署合适的 HTTPS/WSS 地址；不能将 `127.0.0.1` 用作远端服务器地址。

## 房间管理

```powershell
& $lk room create demo --empty-timeout 300
& $lk room list
& $lk room update demo --metadata '{"purpose":"test"}'
& $lk room participants demo
& $lk room participant demo --identity publisher
& $lk room mute-track demo --identity publisher --sid TR_xxx
& $lk room remove-participant demo --identity publisher
& $lk room delete demo
```

管理命令输出 JSON；加入房间时每行输出一个 JSON 事件，包含发布确认、订阅、连接状态、数据消息及结束统计。
`joined` 表示信令鉴权成功，`connected` 表示所需 WebRTC 传输连接成功。失败返回非零退出码，Ctrl+C 返回 130。

## 发布和订阅

先运行订阅端，再运行发布端：

```powershell
# 无声订阅并记录各轨道，无需声卡
& $lk room subscribe demo --identity viewer --record .\recordings

# 无需 FFmpeg 的视频图案和 440 Hz 音频
& $lk room publish demo --identity publisher --publish-demo --publish-tone

# 播放订阅音频；默认不会使用扬声器
& $lk room subscribe demo --identity listener --audio-playout

# 原生窗口显示视频，并播放音频（无需浏览器或 FFmpeg）
& $lk room subscribe demo --identity desktop-viewer --video-playout --audio-playout

# 可同时显示和记录，省略 --audio-playout 则静音观看
& $lk room subscribe demo --identity recorder --video-playout --record .\recordings

# 麦克风发布
& $lk room publish demo --identity speaker --microphone

# 双向收发：room join 同时支持发布与订阅参数
& $lk room join demo --identity duplex --microphone --audio-playout
```

`--duration 10` 表示连接成功后 10 秒退出。`--no-subscribe` 关闭自动订阅；
重复 `--subscribe-track TR_xxx` 可只订阅指定轨道。默认订阅所有轨道，包括后加入的发布者。

`--video-playout` 使用 SDL2 渲染 WebRTC 解码后的 YUV 视频，每条视频轨道一个可缩放窗口，
第一帧到达时才开窗。窗口保持视频宽高比，支持分辨率变化和视频旋转信息；取消订阅后关闭对应窗口。
关闭任意视频窗口或按 Esc 会正常离开房间并结束命令；Ctrl+C 和 `--duration` 也会清理窗口及录制文件。
可以与 `--audio-playout`、`--record` 和 `--subscribe-track` 组合，不能与 `--no-subscribe` 组合。
显示端仅保留最新一帧，避免画面积压；显示帧数可能少于收到的帧数，录制仍走独立的逐帧写入路径。
`video-window` 事件显示实际使用的 SDL 渲染后端，`video-format` 显示画面尺寸，`video-playout-summary` 显示显示帧数。
没有 `--video-playout` 时不会初始化窗口系统。Windows 构建静态链接 SDL2，无需单独安装 SDL2.dll；
Linux 构建需要 SDL 所需的窗口系统开发库（例如 X11/Wayland），尚未在 Linux 上验证。

文件和网络输入需要 FFmpeg、ffprobe 在 PATH，或传 `--ffmpeg C:\path\ffmpeg.exe`，ffprobe 放在同一目录。

```powershell
# 自动探测第一条音频流和第一条视频流，可重复 --publish
& $lk room publish demo --identity file --publish '.\sample.mp4' --exit-after-publish
& $lk room publish demo --identity file --publish '.\audio.ogg' --publish '.\video.ivf' --loop
& $lk room publish demo --identity camera --publish 'rtsp://127.0.0.1:8554/live'

# 已编码的 TCP 输入：FFmpeg 监听输出，CLI 连接并解码
# 单独终端：ffmpeg -re -i sample.mp4 -an -c:v libx264 -tune zerolatency -listen 1 -f h264 tcp://127.0.0.1:9000
& $lk room publish demo --identity tcp --publish 'h264://127.0.0.1:9000'

& $lk room join demo --identity sender --publish-data 'hello' --topic greeting --duration 5
```

`h264://`、`h265://` 接收 Annex-B；`vp8://` 接收 IVF；`opus://` 接收 Ogg Opus（不是裸 RTP）。
输入先解码，然后由 WebRTC 重新编码。视频默认 640×360/30fps/VP8，支持 `--width`、`--height`、`--fps`、`--codec`；
可选编码器取决于 WebRTC 构建，分辨率必须为偶数。音频统一转为 48 kHz/16-bit/单声道，由 WebRTC 编码为 Opus。
数据接收事件的 `payloadBase64` 是 URL-safe Base64。

## 录制与当前边界

- `--record` 写入每个订阅轨道独立的 WAV / Y4M 文件，不覆盖已有同名文件。视频分辨率或音频格式变化时分段。
- WAV 在正常退出和错误清理时补齐头部；长音频在 RIFF 大小上限前分段。强制结束进程可能留下不完整文件。
- Y4M 使用名义 30 fps，不保留原始 RTP 时间戳。各轨道分别录制，目前不提供同步封装的 MP4 或 A/V 同步时间线。
- 文件音视频分别由 FFmpeg 解码进程驱动；网络输入包含音频和视频时，会建立两个输入连接。裸 TCP 按指定 codec 直接连接，不做预探测。
- 已实现首次连接、双 PeerConnection、ICE 缓存/交换、心跳和有序关闭；断线或服务器要求重连时明确报错退出，自动恢复尚未实现。
- 暂无 simulcast/SVC、端到端加密、相机/桌面采集、Cloud/Agent 部署等原版完整命令集。H.265 输入可解码转码，是否能以 H.265 发布由后续编码器集成决定。
- 未指定 `--microphone` 或 `--audio-playout` 时，使用实际驱动音频解码的无声输出设备，适合服务器和无声录制。

## 验证

CTest 覆盖 Token/URL 基础逻辑、Protobuf wire 字段、子进程参数、真实 PCM 源，以及原生 PeerConnection/SDP 创建。
端到端脚本创建自己的本地服务实例和房间，验证管理 API、鉴权失败、原生音视频收发、数据通道、非静音 WAV、Y4M 和 Unicode 文件路径：

```powershell
python tests/integration.py --lk out/windows-x64/Release/lk.exe --server D:\path\livekit-server.exe
python tests/stream_integration.py --lk out/windows-x64/Release/lk.exe --server D:\path\livekit-server.exe
```

所有子进程会清理，测试日志和录制保存在 `out/integration/<时间>/`。迁移后的验证结果见根目录 `VALIDATION.md`。

## 结构

| 路径 | 职责 |
| --- | --- |
| `cli/main.cpp` | 命令解析和房间 API 操作 |
| `src/auth.cpp` | HS256 JWT、URL、JSON 事件 |
| `src/net.cpp` | HTTP/Twirp 和 WSS 信令 |
| `src/room.cpp` | LiveKit 双 PeerConnection、协商与轨道生命周期 |
| `src/media.cpp` | FFmpeg 输入、音视频源、订阅录制 |
| `src/internal/lk/headless_audio.hpp` | 无声音频播放时钟，驱动订阅解码 |
| `src/process.cpp` | Windows/POSIX 子进程与管道；不经过 shell |
| `protocol/livekit.proto` | 保留官方字段编号的 Protobuf 子集 |

协议参考快照保留在 `protocol/upstream_*.txt`；未实现的 protobuf 字段由运行库忽略。
