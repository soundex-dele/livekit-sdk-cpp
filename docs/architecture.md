# Current SDK architecture

Source-aligned on 2026-11-03. Public APIs are [MediaEngine](media-engine.md) and [Room](livekit-client-design.md). The [legacy implementation was removed](legacy-engine-removal.md).

`client::MediaEngine` owns a `client::Room` and publication/subscription media pipelines. Each pipeline owns a GraphEngine with capture/input, codec, synchronization, application output, recording or audio output nodes. LocalTrack/RemoteTrack handles bridge encoded frames into Room. Removing a track closes only that pipeline.

The optional `media_graph.hpp` / `LiveKit::MediaEngineGraph` extension lets a synchronous builder modify the default topology or replace it while preserving the engine-owned Room port. GraphEngine drives node lifecycle; custom producers use activation and quiescence hooks. Validation rejects shared/initialized nodes, disconnected or invalid edges and media cycles. Custom graph errors retain node/track context in MediaTrack snapshots. The ordinary MediaEngine header only forward-declares the extension options.

`client::Room` delegates to RoomSession. RoomSession owns participant/publication state and coordinates the internal Rust-style RtcEngine and RtcSession. RtcSession owns SignalClient and native PeerTransport resources. These reside in `src/backends/livekit` and never depend on media graphs, capture devices, actual codec implementations or renderers.

Controls are synchronous. connect returns bool, valued operations return Result<T>, void operations log directly at their callsites. RoomObserver notifications remain asynchronous and serial; callback threads may query snapshots but must not reenter synchronous controls. Application media callbacks use a bounded dispatcher.

close cancels outstanding room work, stops media producers and fences callbacks before releasing resources. A closed engine is not reused; construct another instance to reconnect explicitly. Recovery of an active connection remains internal and uses the server refresh token.

`src/media_nodes` contains reusable codecs, capture, playout and recording; `src/media_graph` contains common graph coordination helpers. `rtc` names in the remaining media-node value APIs do not refer to the removed room engine. Native transport code is under `src/backends/livekit/media`; FFmpeg codecs and the PCM resampler are media-node helpers.

RPC and stream registries, data-track state and bounded service workers live in `src/backends/livekit/data`. Their lifetime spans RtcSession replacement; interruption cancels in-flight work while method/topic registrations remain. Participant methods block only the caller, leaving RTC polling available to deliver responses. `ConnectionOptions` carries E2EE, synchronous token acquisition and region policy. See [room services](room-services.md).
