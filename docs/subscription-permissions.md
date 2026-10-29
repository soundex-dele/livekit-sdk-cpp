> 历史接口记录：本文的旧 Room/Participant/帧流 API 已删除，不适用于当前构建。当前用法与支持范围见 [编码媒体 API](rtc-encoded-api.md) 和 [实施清单](rtc-backend-refactor.md)。

# 订阅权限与接收质量

发布者通过 `RoomOptions.subscription_permissions` 设置首次发布前的权限，通过 `LocalParticipant::set_subscription_permissions` 替换运行时策略。`all_participants=false` 且 participants 为空表示拒绝所有人。

```cpp
livekit::sdk::RoomOptions options;
options.subscription_permissions = livekit::sdk::SubscriptionPermissions{false, {}};
// 入会并发布后，只允许 viewer 订阅指定本地轨道。
room.local_participant().set_subscription_permissions({false, {
    {"viewer", false, {publication.info().client_id}}
}}).get();
```

使用稳定的本地 `client_id`，而不是服务器 SID。运行时设置必须引用已确认的本地发布；完整重新入会会按新 SID 重建权限，尚未确认或已删除的源保持拒绝。首次或恢复发布前先发送当前策略，轨道确认后补入新 SID，避免恢复过程扩大访问范围。设定 `all_tracks=true` 时列表必须为空，表示允许该参与者订阅当前及之后发布的所有轨道。

设置完成表示信令提交，没有服务端成功 ACK。对端通过 `RoomCallback.on_subscription_permission_changed(participant_sid, track_sid, allowed)` 观察官方权限更新；实际订阅/退订仍使用现有轨道回调。策略最多 64 个参与者，每人 128 个轨道，总标识符字符串不超过 32,000 字节。权限不控制普通数据消息的接收。

远端视频可调用 `set_video_quality(VideoQuality::low/medium/high)`；与 `set_video_dimensions` 最后调用者优先。请求保存在恢复设置中，但实际层级取决于发布者与 SFU。此接口不表示本 SDK 已实现多层 Simulcast 发布。
