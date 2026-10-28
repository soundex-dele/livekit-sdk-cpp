# Live video demand control

Set `MediaSpec::dynacast = true` to let LiveKit's aggregate subscription demand pause and resume a live video source. It is off by default. Supported sources are application-fed I420, generated video, camera and screen capture. Audio and file/network playback are rejected: stopping and restarting a playback reader would lose its position.

```cpp
livekit::sdk::MediaSpec media;
media.width = 320;
media.height = 180;
media.video_source = std::make_shared<livekit::sdk::I420VideoSource>(
    livekit::sdk::I420SourceOptions{320, 180});
media.dynacast = true;
auto publication = room.local_participant().publish_track(media, {"front-camera"}).get();
```

The SDK consumes the server's `SubscribedQualityUpdate` (wire field 14). For the currently published codec, at least one enabled quality keeps the single-layer source running. All qualities disabled sets its native RTP sender inactive and stops that track's GraphEngine pipeline, including capture and encoding. The source remains attached to the publication; an I420 producer sees `accepting == false` and backpressure. An enabled update restores the sender and starts the same pipeline after transport readiness, preserving the publication SID. Capture pause also pauses that source's local preview.

Updates for unknown or old SIDs, audio, non-opted-in sources, unrecognized qualities, or a different codec are ignored. Repeated identical demand does not repeatedly restart capture. The option survives full rejoin as part of the desired publication, while old server SID mappings are cleared. Rejoin behavior with dynacast still needs its dedicated fault-injection acceptance; ordinary source-rejoin regression is not that proof.

The server debounces OFF (5 seconds by default in the tested Go checkout). The initial publication sends bootstrap frames so the SFU can establish its receiver and codec before demand becomes available; this is not a guarantee of zero upload before the first viewer. After the initial aggregate OFF, capture and video RTP stop. Server connectivity must be established to receive demand changes.

This is **single-layer source enable/disable**. It does not implement resolution/layer selection, backup-codec publication, automatic H.265→H.264 regression, or board-specific hardware shutdown/feedback. Those remain separate requirements. Applications must explicitly enable this policy for each intended live publication.

`livekit_uav_requirements URL dynacast h264|h265 OUTPUT_DIR` verifies generated I420 through the real Go server. It covers 0→1→2→1→0→1 viewers; keeps the remaining viewer for longer than the server debounce; checks source ownership/consumption and retained outbound RTP statistics over two intervals; and verifies decoding resumes under the same SID. The normal Python UAV runner includes both codec cases. No desktop or capture devices are used.
