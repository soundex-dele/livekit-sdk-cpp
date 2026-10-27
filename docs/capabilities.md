# 能力查询

`sdk_capabilities()` 不初始化 WebRTC、不打开设备、不发网络请求。它报告当前公开 RTC API 的实现状态及可选择的信令版本。`supported` 表示 SDK 已实现该功能，不能代替当前服务端、远端参与者、设备或平台的验收结论。

```cpp
const auto local = livekit::sdk::sdk_capabilities();
if (local.support(livekit::sdk::ClientFeature::rpc) ==
    livekit::sdk::CapabilitySupport::supported) {
  // 可以使用本地 RPC API；调用仍可能因远端没有该方法而失败。
}
const auto current = room.capabilities();
```

`Room::capabilities()` 返回线程安全的当前会话快照：generation、是否 connected、已接受 JoinResponse 使用的信令版本、subscriber-primary、实际配置的 relay-only，以及当前 dual-PC 模式。尚未收到 JoinResponse、完整 rejoin 等待期间和断开后，未知字段保持 `nullopt`；恢复原会话时保留已知策略并将 connected 置 false。不能把 relay-only 配置当成实际选中 TURN candidate 的证明。

`server` 中的版本、区域和 advertised_protocol 来自官方 ServerInfo；老服务端版本可来自已废弃的 server_version 字段。服务端声明 protocol 18 不会将本地协议 12 自动升级到 18。`enabled_publish_codecs` 是服务器发布策略列表，不是某条轨道实际协商、编码或解码的 codec。服务端省略列表时为 `nullopt`，不会猜测为全部允许或全部禁止。

ServerInfo 不提供通用 RPC/E2EE/DataTrack 能力确认，因此 `server.support(feature)` 当前通常为 `unknown`。`unknown` 与 `unsupported` 必须分开处理。不要根据版本字符串或一次普通数据发送成功推断其它能力。

完整 rejoin 清除旧节点信息，新的 JoinResponse 重建快照；resume 有新 ServerInfo 时替换节点信息，未提供时保留。RoomMoved 不携带 ServerInfo，因此清除旧节点声明，保留仍适用的连接配置，等待后续入会提供新证据。查询不泄漏 Token、ICE 凭据、服务端 debug_info 或节点 ID。

本接口不声称提供硬件编码器列表、每轨实际 codec/SDP 协商结果或物理设备能力；这些仍需后续专用接口。测试覆盖声明/未知状态、版本不混淆、恢复信息替换及迁移清理，并通过官方服务端入会/断开集成验证。
