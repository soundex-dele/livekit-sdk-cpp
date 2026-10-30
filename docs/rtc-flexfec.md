# FlexFEC 视频保护

原生 RTC backend 默认启用 `flexfec-03` 协商，适用于 VP8、H.264、H.265 的单层视频编码帧传输。软件编码和外部硬件编码器提供的访问单元共用这条 RTP 传输路径。

```cpp
livekit::sdk::rtc::LiveKitRoomOptions options;
options.enable_flexfec = true;  // 默认值；false 同时关闭本端 offer/answer 中的 FlexFEC
```

此选项属于连接配置，在入会前设置，完整重新入会时继续生效。它启用每个 WebRTC factory 独立的 `WebRTC-FlexFEC-03` 与 `WebRTC-FlexFEC-03-Advertised`，不修改进程全局 field trials。发布轨道选择编码时保留对应的 `flexfec-03` 能力，WebRTC 生成 repair payload type、独立修复 SSRC 和 `FEC-FR` 关联。

对端必须接受 FlexFEC 才会启用保护；对端拒绝时仍可使用原视频编码传输。默认沿用 WebRTC 的自适应保护控制器，冗余量由丢包、RTT、码率等决定，协商成功不表示无损网络上持续产生修复包。此开关不承诺固定冗余比例或任意丢包率下完全恢复。

## SFU 边界

SDK 与 SFU 的上行、SFU 与订阅端的下行是两段独立协商。当前仓库 `livekit-cpp` 的自定义 RTP 转发适配器没有接入 FlexFEC，其协商白名单仍拒绝该 repair codec。仅开启 SDK 不会使该 SFU 链路获得 FEC 保护，也不能直接转发修改过 SSRC/序列号的媒体所对应的原始修复包。

SDK 最初只实现原生传输和协商。随后已在指定的 Go 工作区 `D:\workspace\livekit` 实现发布端到 SFU 的接收恢复，以及 SFU 到订阅端的修复包生成，见本文末尾的 Go SFU 上行、下行接入记录。本地 C++ SFU 的边界不变。针对某个服务端部署，应检查实际 answer 和 repair RTP，而不是以 SDK offer 判断保护已生效。

## 无窗口测试

```powershell
cmake --build livekit-sdk-cpp/out/rtc-native-contracts --config Release `
  --target livekit_rtc_flexfec_tests livekit_rtc_native_tests --parallel 4
ctest --test-dir livekit-sdk-cpp/out/rtc-native-contracts -C Release `
  -R '^rtc_(flexfec|native)$' --output-on-failure
```

`rtc_flexfec` 检查默认开启、显式关闭、并存 factory 的选项隔离、非法 codec/fmtp 拒绝，以及实际 offer/answer 的 `flexfec-03` 和发送 SSRC 的 `FEC-FR` 关联。

恢复测试通过两个原生 PeerConnection 建立 ICE/DTLS/SRTP，接收端在解密前丢弃选中的媒体数据包；相同序列号的重传也丢弃，且不协商 RTX。以收到的访问单元 RTP 时间戳匹配丢失帧，再逐字节比较媒体内容（H26x 规范化 Annex-B 起始码），覆盖 VP8/H.264/H.265。对照组丢弃全部修复包时，丢失帧不得恢复；不支持 FlexFEC 的接收端必须能继续接收普通媒体，且不得收到修复包。

为保证每次运行都能触发修复，恢复用例仅在测试注入固定保护率控制器；生产仍使用 WebRTC 自适应控制器。这些用例证明协商、修复包生成、SRTP 传输和丢包恢复接通，不是自适应冗余策略的弱网性能验收。测试使用预生成编码帧，不调用真实采集设备，也不代表设备人工验收或 SFU 端到端 FEC 验收。

## 2026-10-30 Windows Release 验证

- `livekit_rtc_flexfec_tests`、`livekit_rtc_native_tests`、`livekit_rtc_livekit_integration` 编译通过。构建日志位于聚合仓库 `out/flexfec-final-build.log`，集成测试开关追加后的构建位于 `out/flexfec-integration-build.log`。
- CTest `rtc_native`、`rtc_flexfec` **2/2 通过**，合计 32.04 秒。日志：`out/flexfec-final-validation.log`、`out/flexfec-ctest-detail.log`。

| 自动化场景 | 收到的修复包 | 主动丢失的媒体包 | 已恢复 |
| --- | ---: | ---: | ---: |
| VP8 + FlexFEC | 100 | 9 | 9 |
| H.264 + FlexFEC | 100 | 9 | 9 |
| H.265 + FlexFEC | 100 | 9 | 9 |
| H.265，丢弃全部修复包 | 96（全部丢弃） | 7 | 0 |
| H.265，对端关闭 FlexFEC | 0 | 9 | 0 |

成功恢复的三个场景各收到 100 个完整访问单元，内容校验通过。FlexFEC 测试二进制 SHA-256：`4AD3E765D31E6CED6B7B330FEBD01D2984EB9C0602556681DE1224A15341EA9D`。

官方 Go SFU 集成通过多轨传输、重新订阅、独立关闭、静音/恢复、接收需求及订阅权限检查：`out/rtc-livekit-integration/20261030-115916-0m4yo3ra/result.json`。该结果只证明开启新协商能力后的常规互通，不证明该服务器协商或使用了 FlexFEC。

本地 C++ SFU 未通过完整集成：初次使用 Go 配置因不支持 `logging` 字段退出；使用仅含其支持字段的配置后，第四轨在恢复接收需求后始终无帧。开启和关闭 FlexFEC 均复现同样问题，记录分别为 `out/rtc-livekit-integration/20261030-120211-1j3r3trc/`、`out/rtc-livekit-integration/20261030-120453-2804i0zj/`。不能据此宣称 C++ SFU 全链路通过，也没有把失败归因于 FlexFEC。测试程序支持环境变量 `LIVEKIT_TEST_FLEXFEC=0` 进行关闭对照；C++ 配置适配脚本保存在 `out/flexfec-cpp-sfu-integration.py`。

## 实现前的 Go 工作区复验：D:\workspace\livekit

2026-10-30 按用户指定路径重新构建 Go SFU，源码 HEAD 为 `6b6a3e4c5a4cb3dd73b54758db599bc07d07a651`，包含该工作区原有的未提交重传相关修改。本次没有修改 Go 源码。使用 Go 1.27.3，服务端二进制 SHA-256 为 `557f61d2a65a09a539a069051ce29d1c3d42fc424410e1145eeebb4861b30c64`。SDK 集成测试二进制 SHA-256 为 `bf3783b8c45a654db3fdac6b7b32d29287f0eb2595df565cf256adf25b3659bf`。

**常规媒体互通 3/3 通过，FlexFEC 上下行均未协商成功。** 测试通过本地信令代理逐字节转发 WebSocket 消息，解析真实 offer/answer，仅保存媒体协商字段，排除令牌与 ICE 密码；ICE/DTLS/SRTP 媒体直接连接 Go SFU。

| 视频编码 | 多轨媒体及生命周期 | SDK 上行 offer | Go 上行 answer | Go 下行 offer / SDK answer |
| --- | --- | --- | --- | --- |
| VP8 | 通过 | 包含 FlexFEC | 不包含 FlexFEC | 均不包含 FlexFEC |
| H.264 | 通过 | 包含 FlexFEC | 不包含 FlexFEC | 均不包含 FlexFEC |
| H.265 | 通过 | 包含 FlexFEC | 不包含 FlexFEC | 均不包含 FlexFEC |

每个场景覆盖两条 Opus 和两条视频轨道，校验访问单元内容、多轨路由、订阅权限、接收需求切换、静音/恢复、重新订阅及独立关闭。H.264 首轮发现测试把 Go SFU 静音时插入的 `H264KeyFrame2x2` 当成源帧损坏；现仅在对应静音后阶段识别并统计这个精确的 SPS/PPS/IDR 序列，其他内容仍严格比较，并要求源帧继续到达。最终 H.264 用例记录了 5 个此类黑帧。

该 Go 版本的默认 `Room.EnabledCodecs` 没有 `video/flexfec-03`；`pkg/rtc/mediaengine.go` 注册所遍历的 protocol `VideoCodecsParameters` 也不含 FlexFEC。protocol 中存在 FlexFEC 常量，并不等于 SFU 已接入保护。当前只能确认普通媒体回退正常，不能将这次结果描述成经过 Go SFU 的 FEC 修复测试通过。

完整报告：聚合仓库 `out/flexfec-go-sfu-20261030/report.json`，包含源码修改文件哈希及每个场景的结果。各目录均含 `result.json`、`sdp-summary.json` 和客户端/服务端日志：

- VP8：`out/flexfec-go-sfu-20261030/vp8-62ytc9gb/`
- H.264：`out/flexfec-go-sfu-20261030/h264-xt7i1jya/`
- H.265：`out/flexfec-go-sfu-20261030/h265-kezr30_i/`

复验脚本位于 `out/flexfec-go-sfu-20261030/test_with_sdp.py`，使用 `livekit-sdk-cpp/out/client-parity-venv/Scripts/python.exe` 运行，参数为 `--codec vp8|h264|h265`。常规集成脚本也新增 `--codec` 参数和二进制哈希记录。以上均为无窗口自动化结果，不包含真实设备人工验收。

## Go SFU 上行接入（2026-10-30）

该阶段在 `D:\workspace\livekit` 实现 **SDK 发送端 → Go SFU** 的 FlexFEC-03 协商、修复包接收和媒体恢复，支持双 PeerConnection 模式的发布上行。当时订阅下行尚未启用，后续实现见下一节；单 PeerConnection 和 one-shot 信令模式仍不启用这项能力。Go 工作区原有重传配置修改保留。

SFU 从已接受的 SDP 获取 `FEC-FR`、媒体/修复 PT 和扩展映射，在 SRTP 解密后的写入口消费修复包。恢复出的 RTP 进入现有媒体缓冲区，再按普通媒体下发。修复包不成为业务轨道；恢复包不伪造 TWCC 到达反馈。实现支持三种包掩码、序号回绕、乱序、链式恢复，并按 libwebrtc 规则清零用于 XOR 的可变扩展。解码缓存具有数量、包大小和时间上限。

最终测试使用 SDK 原生 WebRTC 发送器与普通订阅器，媒体直接连接重新构建的 Go SFU。Python 只处理 LiveKit 信令。发送出口丢弃选中的加密媒体包，并继续丢弃同序号重传；不协商 RTX。测试专用控制器使用 85/255 保护率、最多三帧保护窗口，200 输入帧产生 66 个修复包，覆盖跨媒体包的 XOR，而非只验证单包冗余。生产仍使用自适应保护率。

| 场景 | 主动丢失媒体包 | 订阅端确认恢复 | SFU 恢复计数 |
| --- | ---: | ---: | ---: |
| VP8 | 10 | 10 | 10 |
| H.264 | 10 | 10 | 10 |
| H.265 | 10 | 10 | 10 |
| VP8，丢弃全部修复包 | 10 | 0 | 0 |
| H.264，丢弃全部修复包 | 10 | 0 | 0 |
| H.265，丢弃全部修复包 | 10 | 0 | 0 |

六个用例均通过；正向用例 SFU 各收到 66 个修复包、拒绝数为零，订阅端逐帧载荷校验通过。负向用例还各拦截了 50 次同序号媒体重传。所有用例均验证上行协商 FlexFEC、下行未协商 FlexFEC。关闭 SDK FlexFEC 的 H.265 多轨生命周期测试也通过，报告为 `out/rtc-livekit-integration/20261030-143334-xwbvr2xh/result.json`。

- Go `pkg/sfu/flexfec`、`pkg/sfu/buffer`、`pkg/rtc` 最终完整测试通过，日志 `out/flexfec-go-sfu-20261030/go-final-tests.log`。早期并行运行曾触发两个协商时序测试失败；单独复查和最终完整运行均通过，未修改这些测试。
- 解码器 fuzz 运行 50,900 次，通过，日志 `out/flexfec-go-sfu-20261030/fuzz.log`。未运行 Go race 检测。
- 更新后的 SDK CTest `rtc_flexfec` 通过，包含原有原生端到端恢复、关闭和回退场景；C++ 格式检查及 Python 编译检查通过。
- 服务端 SHA-256：`a88e31e02243f0aa0a619bb37ced68e8b02139be847162264539c84353e92ad2`。
- 原生恢复测试客户端 SHA-256：`d0a6daf57059f19c774bf12d7c0cf0b19584ee7da1cdc3ca648814da3408147f`。

汇总报告：`out/flexfec-go-sfu-20261030/upstream-report.json`，其中列出六次运行的 `result.json`、SDP 摘要和日志目录。Go 端实现说明位于 `D:\workspace\livekit\docs\upstream-flexfec.md`。

可复现入口（从聚合仓库根目录运行，Python 环境需安装 websockets/protobuf，proto 目录包含 SDK 协议生成的 `livekit_pb2.py`）：

```powershell
python livekit-sdk-cpp/tests/rtc_flexfec_sfu.py `
  --client livekit-sdk-cpp/out/rtc-native-contracts/Release/livekit_rtc_flexfec_tests.exe `
  --server out/flexfec-go-sfu-20261030/livekit-server.exe `
  --proto-dir out/flexfec-go-sfu-20261030/proto --codec h265
# 添加 --discard-fec 运行负向对照；codec 可选 vp8、h264、h265。
```

这些结果是无窗口自动化验证，不包含真实设备人工验收，也不代表自适应冗余策略的弱网性能评估。

## Go SFU 下行接入（2026-10-30）

`D:\workspace\livekit` 现已默认启用 **Go SFU → SDK 订阅端** 的 FlexFEC-03。双 PeerConnection 的订阅 offer 包含修复 codec 和独立 SSRC 的 `FEC-FR` 关联；订阅端接受后才生成修复包。不支持 FlexFEC 的订阅端继续接收普通媒体。通过 `rtc.enable_downstream_flexfec: false` 可单独关闭下行，SDK 仍由入会前的 `enable_flexfec` 控制。

SFU 在媒体完成 SSRC、序号、时间戳、载荷和扩展改写且成功发送后，使用最终 RTP 生成新的 XOR 修复包，并通过同一 pacer 发送。当前为固定策略：最多 5 个媒体包一组，未满组在 100 ms 内刷新，每组可恢复一个丢包；媒体不等待组满。修复流具有独立序列空间、RTCP SR/CNAME 和带宽统计，层选择预算包含修复开销。RTX、探测、填充和超过 1400 字节的媒体 RTP 不纳入保护；较大的媒体仍正常转发。此策略与 SDK 发布端使用的 WebRTC 自适应控制器不同。

最终测试在订阅端 SRTP 解密前注入下行丢包，不协商 RTX，并持续丢弃相同序号重传。恢复通过原始访问单元时间戳和逐字节载荷比较确认。每个编码分别运行恢复、丢弃修复包、接收端关闭能力三个独立场景：

| 编码 | 开启下行：丢失 / 恢复 | 丢弃修复包：丢失 / 恢复 | 关闭接收能力：修复包 / 恢复 |
| --- | --- | --- | --- |
| VP8 | 10 / 10 | 10 / 0 | 0 / 0 |
| H.264 | 10 / 10 | 10 / 0 | 0 / 0 |
| H.265 | 10 / 10 | 10 / 0 | 0 / 0 |

九个下行场景全部通过；恢复场景各收到 199 个完整访问单元，对照场景各收到 189 个。相同服务端的 H.265 上行回归也恢复 10/10，开启 FlexFEC 的 H.265 多轨生命周期集成通过。

- SDK CTest `rtc_flexfec` 通过（24.44 秒），C++ 格式、Python 编译及 diff 检查通过。
- Go FlexFEC 专项测试通过；`sfu/flexfec`、`sfu/pacer`、`sfu`、`rtc` 完整包测试通过。`config` 完整包的既有 TURN 文件权限用例在 Windows 失败；FlexFEC 配置用例通过。未运行 Go race 检测。
- 服务端：`out/flexfec-go-sfu-20261030/livekit-server-downstream.exe`，SHA-256 `876342e9288ddbc7f7a3ad2b6598e6dce47e0387941122e35fdc4c235f4ba690`。
- 原生恢复客户端 SHA-256：`1ed90c807383612db5f9426b1dc02171e56df4a2217493d43f939ac7288a2993`。
- 汇总报告：`out/flexfec-go-sfu-20261030/downstream-report.json`；Go 实现说明：`D:\workspace\livekit\docs\downstream-flexfec.md`。

从聚合仓库根目录复现：

```powershell
python livekit-sdk-cpp/tests/rtc_flexfec_sfu.py `
  --client livekit-sdk-cpp/out/rtc-native-contracts/Release/livekit_rtc_flexfec_tests.exe `
  --server out/flexfec-go-sfu-20261030/livekit-server-downstream.exe `
  --proto-dir out/flexfec-go-sfu-20261030/proto --codec h265 --downstream
# 添加 --discard-fec 或 --receiver-no-fec；codec 可选 vp8、h264、h265。
```

以上是无窗口自动化验证；未进行真实设备人工验收，也未验证单 PeerConnection、one-shot 或自适应弱网性能。
