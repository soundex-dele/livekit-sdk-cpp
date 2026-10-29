# RTC 控制链路简化（2026-10-29）

本次保留 RoomEngine、媒体图和 LiveKit 后端的职责边界，优化内部等待和重复状态管理。
应用使用的 RoomEngine / RtcEngine 公开接口保持不变。

## 调整

1. **解绑不再阻塞房间控制线程。** 内部 `RoomTransport::remove()` 返回
   `std::future<void>`，LiveKit 工作线程完成原生解绑后兑现结果。RoomEngine 在现有
   控制循环中完成关闭请求；等待期间仍能处理其他轨道。重复关闭等待同一端点的
   解绑，同名替代轨道不受旧句柄影响；退房的 `stop()` 提供最终关闭屏障。
   待解绑记录及关闭等待者分别限制为 1024，避免积压无限增长。后端实现和测试替身
   需要遵循新的内部接口；future 销毁不得阻塞，`stop()` 返回前必须完成原生清理。
2. **媒体操作复用图操作的 future。** 删除 RtcMediaEngine 的 pending 列表、二次
   promise/future 包装和 `finish_pending()`。PipelineEngine 在图操作失败、取消或
   销毁时先执行内部媒体清理，再完成失败 future，保证调用方观察到完成时输入租约
   已释放、媒体状态已更新。端点就绪和图启动仍是两个不同阶段。
3. **事件准入策略共用。** `bounded_events.hpp` 统一统计事件按目标合并、容量限制
   和控制事件优先淘汰统计事件的规则。两层保留各自的成员状态合并及终止语义：
   RoomEngine 的状态提示按 epoch 合并，Transport 仅替换相邻的状态快照。

线程数量没有强行合并：图关闭需要等待媒体 worker；应用回调使用独立通知队列。
把这些等待移进房间控制线程会重新阻塞其他轨道。RoomEngine 的发送、超时和
传输轮询循环仍然存在；本次没有引入额外轮询线程，也不宣称已经完成性能基准测试。

## 自动化验证

Windows / MSVC Release：

- 后端、流水线、媒体引擎、架构测试目标和三个原生集成客户端编译通过。
- `rtc_backend`、`rtc_pipelines`：2/2 通过。
- `rtc_architecture`、`rtc_media_engine`：2/2 通过。
- 新增回归覆盖延迟解绑时其他轨道继续工作、重复关闭屏障、同名轨道隔离、解绑
  异常传播、退房完成未结束解绑、两类事件的统计合并与控制事件溢出，以及后端
  拒绝发布时媒体清理先于 future 完成。已有媒体取消、图启动失败测试继续通过。
- 隔离 SFU 综合媒体集成最终通过：多轨收发、重订阅、独立关闭、取消待发布轨道、
  成员信息、静音、接收需求和订阅权限。
- 隔离 SFU 多图集成通过：独立关闭、退房重入、服务实例隔离。
- 隔离 SFU 断线恢复通过：两次完整重新入房、关闭轨道不重放、旧 epoch 拒绝、
  静音恢复和终止退出。
- 新增公共策略文件通过 clang-format 20.1.8 检查；修改区域已格式化，SDK
  `git diff --check` 通过。

测试过程中，首次完整媒体构建因另一个构建进程占用同一静态库而失败，等待释放后
重新构建通过。综合媒体集成第一次出现单路视频接收超时，第二次在入房时收到
SFU 的 HTTP 500（`could not find any available nodes`）；增加失败调用位置诊断后
第三次通过，没有放宽断言或超时。前两次失败原因未全部归因，不能据此宣称测试
稳定性问题已解决。

本次均为无窗口自动化验证，没有进行真实设备人工验收，也未验证 macOS/Linux。

## 复现与证据

在聚合仓库根目录执行：

```powershell
cmake --build livekit-sdk-cpp/out/rtc-backend-contracts --config Release --target livekit_rtc_backend_tests livekit_rtc_pipeline_tests --parallel 3
ctest --test-dir livekit-sdk-cpp/out/rtc-backend-contracts -C Release -R '^rtc_(backend|pipelines)$' --output-on-failure
cmake --build livekit-sdk-cpp/out/rtc-media-contracts --config Release --target livekit_rtc_media_engine_tests livekit_rtc_architecture_tests livekit_rtc_livekit_integration livekit_rtc_engine_integration livekit_rtc_rejoin_integration --parallel 3
ctest --test-dir livekit-sdk-cpp/out/rtc-media-contracts -C Release -R '^rtc_(media_engine|architecture)$' --output-on-failure
python livekit-sdk-cpp/tests/rtc_livekit_integration.py --client livekit-sdk-cpp/out/rtc-media-contracts/Release/livekit_rtc_livekit_integration.exe --server out/uav-sfu-go/livekit-server.exe
python livekit-sdk-cpp/tests/rtc_livekit_integration.py --client livekit-sdk-cpp/out/rtc-media-contracts/Release/livekit_rtc_engine_integration.exe --server out/uav-sfu-go/livekit-server.exe
livekit-sdk-cpp/out/client-parity-venv/Scripts/python.exe livekit-sdk-cpp/tests/rtc_rejoin_integration.py --client livekit-sdk-cpp/out/rtc-media-contracts/Release/livekit_rtc_rejoin_integration.exe --server out/uav-sfu-go/livekit-server.exe
```

断线恢复脚本需要安装 websockets 的 Python 环境；本机使用上面的既有虚拟环境。

- [后端构建日志](../../out/rtc-simplification-backend-build.log)
- [完整媒体重试构建日志](../../out/rtc-simplification-media-build-retry.log)
- [综合媒体最终结果](../../out/rtc-livekit-integration/20261029-144431-erz3x3v_/result.json)
- [多图结果](../../out/rtc-livekit-integration/20261029-143834-vlvde_6p/result.json)
- [断线恢复结果](../../out/rtc-rejoin-integration/20261029-143920/result.json)
- [首次综合媒体失败日志](../../out/rtc-livekit-integration/20261029-143805-l66dgz69/client.stderr.log)
- [第二次 SFU 启动阶段失败日志](../../out/rtc-livekit-integration/20261029-144139-5_ncs910/server.log)
