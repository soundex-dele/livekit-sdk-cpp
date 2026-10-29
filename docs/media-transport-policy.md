> 历史接口记录：本文的旧 Room/Participant/帧流 API 已删除，不适用于当前构建。当前用法与支持范围见 [编码媒体 API](rtc-encoded-api.md) 和 [实施清单](rtc-backend-refactor.md)。

# Direct media transport policy

`RoomOptions::media_transport` constrains both native WebRTC PeerConnections for
the entire logical Room session. The default `MediaTransport::automatic` retains
the existing behavior. Use separate Room objects for UDP live media and TCP
playback; tracks sharing a BUNDLE connection cannot choose different protocols.

```cpp
livekit::sdk::RoomOptions live;
live.media_transport = livekit::sdk::MediaTransport::udp_only;
livekit::sdk::RoomOptions playback;
playback.media_transport = livekit::sdk::MediaTransport::tcp_only;
```

Both strict modes disable TURN, even when `disable_turn` is left at its default.
They reject `relay_only` and server `force_relay` rather than silently overriding
the direct-connect requirement. `udp_only` disables TCP candidates and TCP port
allocation. `tcp_only` disables UDP and UDP STUN allocation while retaining native
active ICE-TCP. The TCP mode means direct DTLS/SRTP over ICE-TCP to the SFU; it does
not mean signaling WebSocket TCP or TURN/TCP.

The allocator also disables relay allocation for the existing `disable_turn`
option. Automatic transport with TURN enabled remains unchanged.

Initial join, refreshed ICE configuration during resume, and full rejoin use the
same policy. Recovery to another endpoint does not relax it. If the endpoint has
no reachable candidate of the requested protocol, connection/recovery fails under
the existing time and retry budgets. No per-track policy or runtime policy switch
is provided. This cannot make an unreachable direct route reachable.

The UAV runner tests H.264/H.265 decoded media over each protocol, refuses the
opposite-protocol-only server after successful signaling, and checks selected ICE
pairs on both media legs. Its simultaneous test runs UDP H.265 plus three-party
Opus and an independent TCP H.265 instance on the same host. Video remains synthetic
320×180 I420; the TCP source is not a recorded-file or encoded-passthrough test.

```powershell
python tests/uav_requirements.py --server-source D:/workspace/livekit --tcp-repeats 3

# Server has both UDP and TCP available; the client constrains the protocol.
python tests/reconnect_integration.py --server PATH_TO_CURRENT_GO_SERVER `
  --client out/windows-x64/Release/livekit_reconnect_integration.exe `
  --scenario resume --media-transport tcp
python tests/reconnect_integration.py --server PATH_TO_CURRENT_GO_SERVER `
  --client out/windows-x64/Release/livekit_rejoin_integration.exe `
  --scenario rejoin --media-transport udp
```

These headless tests do not certify physical network changes, ARM64 hardware,
mobile/Web support, production ICE-TCP reachability, latency, capacity or 8-hour
stability. See the workspace UAV validation reports for actual execution results.
