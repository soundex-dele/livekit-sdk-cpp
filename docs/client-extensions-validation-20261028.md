# RTC 客户端扩展统一验证（2026-10-28）

本次在功能代码完成后统一验证，按测试暴露的问题修正并复测。只扩展 RTC 客户端；未修改服务器 SDK、Go 对照仓库或服务端。实现入口、资源约束和非等价项见 [客户端扩展](client-extensions.md)。本文不将本地实现等同于所有平台、codec 组合及设备都已验收。

## 基线与构建

- C++：当前 `livekit-sdk-cpp` 工作树，C++20、原生 WebRTC M137、GraphEngine，Windows x64 Release，VS 2022，静态 CRT，FFmpeg RTC codecs 开启。
- Go：`D:/workspace/server-sdk-go`，v2.19.0，revision `743ab2d6b12ff24c2b2f9a0cc208c7afe8602571`；构建后 checkout 仍干净。夹具通过 replace 引用该仓库，未修改其源码。
- Go 编译器：go1.26.10 windows/amd64。测试进程 PATH 使用 `D:/msys64/mingw64/bin` 提供夹具的运行时 DLL。
- 测试服务端：官方 LiveKit Server 1.13.10，`../out/uav-sfu-go/livekit-server.exe`，只启动隔离的本机测试实例。
- 完整构建曾通过：`out/extensions-build7.log`。之后加密 H.264 组帧修正及动态尺寸测试的最终 SDK/测试目标构建通过：`out/extensions-build-final.log`。
- 最终构建显式覆盖 SDK、扩展单测/互通、SDK 单测/集成、媒体图、resume/rejoin、区域和 RoomMoved 目标。其他会话同时构建 conference demo 曾造成 `room.obj`/`ui.obj`/demo exe 占用；失败保留在 build8/build9 等日志，不计作通过，也未终止其它任务。
- 最终集成使用 `out/extensions-validated-bin/` 中的二进制快照，避免共享构建目录在测试过程中被重新链接。完整文件哈希在该目录的 `manifest.json`。

| 固定产物 | SHA-256 |
|---|---|
| 官方测试服务端 | `c42072801590a2d1178c0b1bb7c32604b5c3200f307f502bf14d3071b37d4a98` |
| C++ 扩展互通客户端 | `9ea40394c7a05cfcafafa2c0d397b1f22e19f11162d907b3ee96f4fa289a7878` |
| Go 互通夹具 | `1d686e1547563211e1f0a54e49275dbe88131b5c59e79dd916af207e165c5ab3` |

## 自动化结果

`ctest --test-dir out/windows-x64 -C Release --output-on-failure`：**14/14 通过，15.28 秒**。记录：`out/extensions-ctest-final.log`。

覆盖 protocol descriptor、screen capture 契约、remote control 契约、媒体图、video frames、core、client extensions、SDK、传输策略、FFmpeg codec、CLI、native WebRTC、video playout 和无窗口 demo render。没有使用桌面自动化。

新增契约重点包括：UTF-8 跨块、raw-DEFLATE/膨胀限制、流公平调度与取消；AES-GCM 标准向量、认证失败、轮换；DataTrack 分片乱序/重复/加密/rejoin；编码源代次及关键帧恢复、RTP 回绕/SSRC、RTCP SR 时间映射；动态 RGBA 尺寸/旋转/实际字节预算；非整块 PCM 和有界漂移补偿；控制回调排序和异常。媒体图测试实际编码三个 VP8 层并分别解码，检查逐层停编码，以及合成音画时钟映射、同步组隔离和重置。

六种扩展互通全部通过，结果目录时间均为 `20261028-142139`：

| 模式 | 实际验证 | 结果路径（相对 SDK 根目录） |
|---|---|---|
| plain | 双向 100,000 字节中文/emoji 文本、300,000 字节文件、40,000 字节 DataTrack，逐字节及用户时间戳校验 | `out/extensions-plain/20261028-142139/result.json` |
| encrypted | 上述文本/文件/数据帧均采用 E2EE | `out/extensions-encrypted/20261028-142139/result.json` |
| single | 协议 17 单 PC、Join 预发布 VP8、Go 收到 RTP、数据与流同时互通、原生 packet observer 有输出 | `out/extensions-single/20261028-142139/result.json` |
| backup | 主 H.265、仅接受 H.264 的 Go 订阅者触发同 SID 的备用 H.264；PLI 后保存完整关键帧，FFmpeg 实际解码 | `out/extensions-backup/20261028-142139/result.json` |
| encoded | H.264 文件按时间原码发送、Go 重组后回送、C++ 编码输出及 160×120 像素输出、FFmpeg 解码 | `out/extensions-encoded/20261028-142139/result.json` |
| media-e2ee | 上述 H.264 媒体双向加密/解密及实际解码，数据流和 DataTrack 同时加密互通 | `out/extensions-media-e2ee/20261028-142139/result.json` |

每种模式还执行 Go↔C++ Opus/H.264/H.265 加密格式向量。只有 H.264 在本轮做了真实加密视频双向解码；不能将 Opus/H.265 的向量通过描述为对应真实加密媒体链路已验收。

最终生命周期和既有行为回归全部通过，结果目录时间均为 `20261028-142232`：

| 回归 | 证据 |
|---|---|
| 两轮 resume、媒体继续、TokenProvider、取消、三组 publisher ICE credentials | `out/reconnect-integration/20261028-142232/result.json` |
| 两轮完整 rejoin、稳定 source ID、旧句柄拒绝、已删除轨道不重发、终止离开 | `out/rejoin-integration/20261028-142232/result.json` |
| 首次信令失败和 Join 后媒体停滞切换、终止错误不切换、总期限/取消/信任限制 | `out/region-integration/20261028-142232/result.json` |
| RoomMoved 协议注入后真实目的房间重连/媒体、目标 Token、旧句柄隔离 | `out/room-moved-integration/20261028-142232/result.json` |
| 三轨动态发布/订阅/取消、两次会话重用、定向数据、WAV/Y4M 非空及有效内容 | `out/sdk-integration/20261028-142232/result.json` |
| 双向单声道 PCM、RPC echo/错误/未知方法、DTMF、partial/final 转写 | `out/go-interop-integration/20261028-142232/result.json` |

PCM 互通测得 C++→Go 438.5 Hz、Go→C++ 879.5 Hz，各接收 96,000 个采样。RoomMoved 的触发来自 protobuf 注入，不能记为真实 Cloud 无缝迁移成功。

## 统一验证中修正的问题

- raw-DEFLATE 解压有输出但暂缺输入时，不能误判为无进展；补齐压缩 RPC 正向测试。
- 单 PC 预分配接收段尚无 SSRC 时不创建媒体 sink，远端 SDP 生效后再按实际接收器绑定。
- 主轨道 SDP 必须保留备用 codec 参数，服务端才能为不支持主 codec 的订阅者建立备用接收状态；AddTrack 声明本身不足以完成协商。
- 无索引 Annex-B 文件 rewind 失败时重新打开解复用器；关键帧反馈或超时丢帧后丢弃依赖链。
- 原生 H26x 发送在加密前统一起始码，避免 RTP 重组改变认证头。加密 H.264 使用按完整 SPS/PPS/IDR 识别边界的原生 packet buffer，避免解析密文中的 PPS id。
- 会话生命周期清理在 RoomState 锁外、用户回调之前执行，避免 RPC 取消回调重入与独立控制执行器延迟清理。
- Go 夹具按 access unit 聚合 H.264，显式声明回送轨道 GCM，接收使用服务端 SIF sentinel；回送持续到后续关键帧，备用编码从 IDR 起保存。Python 夹具检查两个进程的最终退出码。

## 复现

先完成构建，再运行测试；不要一边链接同一个 exe 一边测试。

```powershell
cmake -S . -B out/windows-x64
cmake --build out/windows-x64 --config Release --parallel 2
ctest --test-dir out/windows-x64 -C Release --output-on-failure

python tests/go_interop/build.py --sdk D:/workspace/server-sdk-go `
  --go out/toolchains/go1.26.10/go/bin/go.exe --compiler-bin D:/msys64/mingw64/bin
$env:PATH = 'D:/msys64/mingw64/bin;' + $env:PATH
out/client-parity-venv/Scripts/python.exe tests/go_interop_integration.py `
  --server ../out/uav-sfu-go/livekit-server.exe `
  --client out/windows-x64/Release/livekit_client_extensions_integration.exe `
  --go-peer out/go-interop-peer/go-interop.exe --extensions media-e2ee
```

将最后一个参数替换为表中的其他模式。测试夹具使用 FFmpeg 生成合成媒体；不采集桌面、摄像头或麦克风。

## 交付边界及后续验收

- 编码媒体支持 VP8/H.264/H.265/Opus；真实加密媒体互通本轮验证 H.264。VP8 E2EE、备用视频与 E2EE 组合、音频备用 codec 不支持，调用显式拒绝。
- RTP 输入是有序 RTP→access unit→原生 WebRTC 重新封包；非逐字节重放，不开放任意 RTCP 发送。packet observer 的 RTP 只包含头和扩展，RTCP 是完整包。
- 三层 VP8 已做真实独立编解码单测，未完成远端三层带宽收缩长测；备用编码实测默认 prefer_regression，其他策略及混合订阅者恢复矩阵仍需专项。
- 动态尺寸、PCM 漂移与音画同步已做合成时钟/队列检查；实际设备播放偏差、AEC 和长期漂移须人工设备验收。
- single-PC 已验证预发布媒体及数据并用，尚未完成其全部多轨、弱网、resume/rejoin 组合矩阵。
- Linux/macOS、硬件 codec、IPv6-only 实网、真实 Cloud 跨区与无缝迁移、长时内存/线程及全面弱网压力测试未执行。
- Go 的底层扩展对象、任意 codec fmtp/帧 ID/媒体用户时间戳不作等价承诺；流支持发送未压缩和接收 raw-DEFLATE，不提供已消费 chunk 的追溯编辑或断点续传。

本报告只将以上自动化通过项记为通过，未将其描述为实际设备或所有 Go API 已完全等价。
