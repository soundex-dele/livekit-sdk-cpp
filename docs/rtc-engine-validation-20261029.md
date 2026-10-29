# RtcEngine 统一入口验证（2026-10-29）

实现与接口见 [RTC 引擎 API](rtc-media-engine.md)。本次是无兼容层的 API 调整：RoomSession 演进为 RoomEngine，RtcMediaEngine 成为内部可选模块，应用统一使用 RtcEngine。

## 自动化结果

Windows / MSVC Release：

| 检查 | 结果 |
| --- | --- |
| 关闭原生后端与媒体模块的纯传输/图构建 | 通过 |
| rtc_backend、rtc_pipelines | 2/2 通过 |
| 完整媒体引擎、CLI、房间与四个音视频示例编译 | 通过 |
| rtc_media_engine | 1/1 通过 |
| 会议、投屏、远控、直播工作台编译 | 通过 |
| 应用 CTest（含 offscreen 渲染） | 21/21 通过 |
| 本机隔离 SFU 端到端测试 | 通过 |
| 新公开接口与内部引擎文件 clang-format 20.1.8 检查 | 通过 |
| 聚合仓库及两个子模块 git diff --check | 通过 |

新增回归覆盖：编码模式未启用媒体时报 unsupported、RoomView 不暴露轨道/房间销毁接口、主管线程限制、退房取消待完成操作、重复 close、close 后禁止 join、退房后重入房间、旧轨道句柄和迟到事件隔离、媒体回调停止屏障、跨房间共享采集在一方退房后继续工作。

集成测试使用本机已有 SFU 二进制、隔离端口与临时配置，验证多轨编码媒体收发、独立关闭、原 RtcEngine 实例退房重入并继续收发，以及旧 RoomServices 关闭/新服务可用。没有启动或操作用户桌面应用。

## 复现命令

在聚合仓库根目录，使用已配置的构建目录：

```powershell
cmake --build livekit-sdk-cpp/out/rtc-backend-contracts --config Release --target livekit_rtc_backend_tests livekit_rtc_pipeline_tests -j 3
ctest --test-dir livekit-sdk-cpp/out/rtc-backend-contracts -C Release -R '^rtc_(backend|pipelines)$' --output-on-failure
cmake --build livekit-sdk-cpp/out/rtc-media-contracts --config Release --target livekit_rtc_media_engine_tests livekit_rtc_engine_integration lk livekit_sdk_example livekit_video_source_example livekit_video_stream_example livekit_audio_source_example livekit_audio_stream_example -j 3
ctest --test-dir livekit-sdk-cpp/out/rtc-media-contracts -C Release -R '^rtc_media_engine$' --output-on-failure
python livekit-sdk-cpp/tests/rtc_livekit_integration.py --client livekit-sdk-cpp/out/rtc-media-contracts/Release/livekit_rtc_engine_integration.exe --server out/uav-sfu-go/livekit-server.exe
cmake --build livekit-app/out/rtc-products-qt6 --config Release -j 3
$env:QT_QPA_PLATFORM='offscreen'
ctest --test-dir livekit-app/out/rtc-products-qt6 -C Release --output-on-failure -j 2
```

## 本机证据

- 集成结果：[result.json](../../out/rtc-livekit-integration/20261029-120840-0q4weh99/result.json)。
- 构建日志：[纯传输](../../out/rtc-facade-core-final-build.log)、[完整媒体](../../out/rtc-facade-media-final-build.log)、[应用](../../out/rtc-facade-app-final-build.log)。
- CTest 完整结果位于相应构建目录的 `Testing/Temporary/LastTest.log`。
- 集成客户端 SHA-256：`9ccde045d007a9709b65d57080b829507b73e8327e70c18592b6e0e2fc65e095`。
- 本机 SFU SHA-256：`c42072801590a2d1178c0b1bb7c32604b5c3200f307f502bf14d3071b37d4a98`。

## 验证边界

以上为自动化检查，不代表真实摄像头、麦克风、扬声器或显示设备已人工验收。本次未验证 macOS/Linux 编译与设备行为。

## PImpl 调整后的复验

RtcEngine 的公开类仅保留 Impl 前置声明和 unique_ptr，状态、内部模块所有权及清理逻辑移入私有实现。公开 API、线程限制、关闭顺序及媒体可选链接方式保持不变。

- Windows Release：纯传输 SDK、完整媒体 SDK、CLI 及四个应用重新编译通过。
- rtc_backend / rtc_pipelines / rtc_media_engine：3/3 通过；应用 offscreen CTest：21/21 通过。
- 隔离 SFU 集成：多轨收发、独立关闭、退房重入和服务隔离全部通过。
- 新增/修改的 PImpl 接口与实现通过 clang-format 20.1.8 检查，SDK diff 空白检查通过。
- 构建日志：[纯传输](../../out/rtc-pimpl-core-build.log)、[媒体与 CLI](../../out/rtc-pimpl-media-build.log)、[应用](../../out/rtc-pimpl-app-build.log)。
- 集成结果：[result.json](../../out/rtc-livekit-integration/20261029-123150-1ug25liw/result.json)。
- PImpl 版本集成客户端 SHA-256：`3855d88508fc09d6cc55a662550b2b32d583cff28a3e520b1b88abd04b0c4aed`。

仍属于自动化验证；未进行真实设备人工验收，未验证 macOS/Linux。
