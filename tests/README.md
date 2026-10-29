# RTC regression entry points

The current API is `RoomEngine` plus separate publish/subscribe nodes and
per-track `RtcEngine` graphs. Tests never require desktop automation.

| Coverage | Current entry point |
| --- | --- |
| Configuration-driven media graphs, real VP8/Opus loopback, borrowed-source cancellation/failure, cross-room shared capture and shared PCM | `rtc_media_engine_tests.cpp` |
| Backend lifetime, quotas, cancellation, epochs, permissions | `room_engine_tests.cpp` |
| Graph construction, independent removal, producer shutdown | `room_engine_tests.cpp` (`LIVEKIT_TEST_RTC_NODES`) |
| Encoded four-track SFU transport, mute, demand, permissions, E2EE | `rtc_livekit_integration.py` + `livekit_rtc_livekit_integration` |
| Per-track engine routing | Same runner + `livekit_rtc_engine_integration` |
| Actual upper-layer audio/video codecs | Same runner + `livekit_rtc_audio_graph_integration` / `livekit_rtc_video_graph_integration` |
| Multi-camera/viewer and three-party audio, UDP/TCP | `uav_requirements.py` + `livekit_uav_requirements` |
| Full rejoin after server leave and signaling loss | `rtc_rejoin_integration.py` + `livekit_rtc_rejoin_integration` |
| Trusted redirects, initial signaling/ICE failover, total deadline, cancellation | `region_integration.py` + `livekit_region_integration` |
| Go data streams, DataTrack, encryption and encoded H264 | `go_interop_integration.py` + `livekit_client_extensions_integration` |
| CLI graphs, recording, conference sessions and aggregate demand | `media_graph_integration.py` |

Python network fixtures require `websockets==15.0.1`; supply the server executable
explicitly with `--server`. The runners start isolated local servers and write
reports under `out/`. CTest runs deterministic contracts and headless checks;
network fixtures are separate commands, not silently included in CTest counts.

## Retired facade coverage

The old `sdk_integration`, `reconnect_integration`, `rejoin_integration`,
`room_moved_integration` C++ clients and their Room/frame-stream helper headers
were removed with the public facade. Dynamic tracks, media routing, cancellation,
full rejoin, recording and Go exchange use the replacements above. Shared Python
fixture primitives are in `sfu_test_support.py`.

Signaling resume, seamless room migration, single-PC, backup codec switching,
simulcast/SVC and automatic metadata replay are not claimed by this migration.
Historical passes of those facade scenarios are not evidence for the new backend.
The old reconnect/room-moved runners and `--sdk-client`, `--rpc-only`, `single`,
`backup` integration modes were removed so they cannot select obsolete binaries.
Full rejoin preserves stable publication IDs and mute intent while replacing
server bindings; applications rediscover remote tracks whose SIDs change.

Synthetic capture, software render and network loopback are automated checks,
not physical microphone/camera/speaker or desktop-interaction acceptance.
