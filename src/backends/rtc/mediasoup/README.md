# mediasoup 适配位置

此目录预留给后续 mediasoup 适配，目前没有可编译实现或构建目标。

适配器应实现上一级的 `RoomTransport` 接口，由公共 `RoomEngine` 管理房间生命周期、
轨道、事件与队列；可复用的 WebRTC 能力位于 `backends/webrtc/`。

mediasoup 不规定应用层房间与信令协议，接入时需确定服务端房间、鉴权、成员发现及
Producer/Consumer 映射。连接配置需与 LiveKit 配置分离，RPC、流、权限等服务需明确
支持范围，不应直接依赖 `rtc/livekit/` 的协议实现。
