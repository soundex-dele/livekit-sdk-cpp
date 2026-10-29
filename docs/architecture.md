# Current SDK architecture

The public configuration API is `rtc/engine.hpp`. It does not include GraphFlow,
WebRTC or FFmpeg headers. `rtc/graph_api.hpp` is an explicit advanced extension:
`engine.graph().publish_graph(...)` and `engine.graph().subscribe(...)` share the
same registry and lifecycle as ordinary media operations.

## Ownership and dependencies

`RtcEngine` owns RoomEngine, a per-track pipeline coordinator and an optional
media module. RoomEngine owns transport state and encoded endpoints, never media
graphs or devices. Each track graph owns its nodes; source/codec/output workers
quiesce before graph teardown. Removing a track does not close the room.

`RTCNodeApi` supplies common node contracts and GraphFlow core. Codec, capture and
output targets depend on it, not RTCEngine. RTCMediaEngine links these lower-layer
targets with RTCEngine. Node consumers constructing graphs must explicitly link
GraphFlow engine; applications using the facade link RTCMediaEngine.

## Execution

RtcEngine owns a serial `vi::TaskQueue` from GraphFlow's `utoolkit/task_queue`.
Public control calls marshal short commands onto that queue. Endpoint completion
notifications schedule graph construction; callers may await returned futures
without driving any loop. There is no RtcEngine or pipeline `poll` API.

Room events use `RoomEngine::set_event_handler`; the facade exposes
`RtcEngine::set_callbacks` for room events and graph failures. There are no engine
`next_event` or `next_failure` methods. Notifications execute on a separate serial
TaskQueue and may issue engine controls. Callbacks must return promptly, must not
destroy the engine, and must not throw. Replacement/removal fences old calls;
registration must be serialized by the application. Callback arguments are
borrowed for that invocation. Session loops may marshal callback copies into
application-owned bounded mailboxes.

Each RTC subscription source owns a worker blocked on its bounded endpoint queue.
Incoming packets wake that worker, which publishes graph events. Publication
feedback uses a separate node worker waiting on endpoint epoch/revision changes.
Neither worker dispatches on transport or control threads. An idle/suspended
source blocks until data, rebind, cancellation or terminal state. One slow source
consumer cannot occupy another track's worker or the room transport thread.

Graph start is two phase: start every node, deliver initial sender feedback on its
worker, then activate capture; subscription ingress starts after downstream
activation. Close cancels and joins ingress/feedback workers before quiescing
codecs and outputs, stopping the graph and detaching transport. No registry lock
is held while joining workers. Custom graph hooks run on the engine task queue;
`TrackGraphSpec::check_health` replaces the old poll hook. A 20 ms delayed task
checks node failures only; it never receives frames, emits feedback or builds
graphs. Media callbacks and graph hooks must not synchronously wait for lifecycle work.

The SDK bounds control admission to 1024 queued calls and coalesces backend
completion notifications. Task closures capture exceptions into futures or the
control failure state because the underlying task queue does not catch them.
Destruction unregisters notifications, executes the teardown barrier and joins the
task queue before destroying state referenced by delayed tasks.

RtcExecutor is now an optional bounded command/future adapter using the same task
queue library; it is not required for engine progress. Its diagnostics count
started/completed commands and command duration. Standard futures have no generic
continuation API, so this adapter uses delayed readiness checks only while a
returned future is outstanding; it does not drive the engine.

Application media callbacks retain their separate bounded output dispatchers.
Slow callbacks discard only their own backlog. They must eventually return:
shutdown joins in-flight delivery. Advanced synchronous graph consumers execute
on the publishing source/codec worker and must not reenter lifecycle operations.

## Frame ownership and timing

`media::FrameView` is the common immutable CPU view for RTC and Player. Its owner
keeps all plane spans alive; format, stride, sample rate, channel count and clock
rate are explicit. RTC `MediaOutput::frame` delivers this view. Player frames
convert with `media::frame_view`. Adapters retain storage without copying;
format conversion remains explicit. GPU surfaces are not represented as CPU spans.

Shared capture distributes immutable raw-frame handles to independent bounded
queues. Encoding and transport remain independent per publication. APM, if
enabled, runs once before shared fanout. One branch closing never closes another
branch's capture lease. Application-owned source objects are not closed by SDK
unpublish. Frame timing is separate from storage, so reconnect retags a branch's
new input without modifying a buffer shared by another room. Shared audio keeps
the source sample clock, including gaps, rather than recomputing RTP time from
callback arrival. Invalid shared queue limits fail synchronously without attaching
a consumer; graph startup failures are reported by the operation's ready future.

## Audio and synchronization

Ordinary audio subscriptions enable NetEq: encoded Opus packets retain sequence
numbers, reorder through the jitter buffer, and produce paced 10 ms PCM with PLC
and adaptive time stretching. Bare AudioCodecNode decoding remains available by
leaving `AudioCodecOptions::jitter_buffer` false. Buffers and idle concealment are
bounded; generation changes reset decoder state. Codec snapshots report concealed,
accelerated and decelerated samples and current jitter-buffer duration.

Microphone inputs default to APM (echo cancellation, noise suppression and digital
gain control); other raw audio inputs opt in with `MediaInput::audio_processing`.
Set its `enabled=false` to bypass processing. Speaker callbacks write their actual
mixed PCM to a bounded AudioRenderReference without allocation or waiting. APM runs
on the capture worker, not in the device callback. Device delay is configurable.
When sharing a capture across rooms, its processing/reference configuration belongs
to that shared capture; do not apply a second APM to each publication branch. The
default reference belongs to one media engine's output. Combining physical output
from multiple engines requires an explicitly shared reference/mixer; there is no
global device manager. Capture generation changes reset processing state.

The native transport negotiates absolute capture timestamps and preserves sender
clock metadata across the encoded boundary. A participant/stream group shares a
presentation clock; video follows actual audio device scheduling when available.
Missing sender clock metadata is explicitly unsynchronized, rather than assuming
unrelated audio and video RTP origins match. Reconnect resets the clock epoch.
`MediaSubscribeSpec` controls synchronization and initial presentation delay.
Audio callbacks and video callbacks receive optional presentation timestamps.

## Backend configuration and capabilities

RoomOptions contains common queue, operation and session settings plus immutable
BackendOptions. LiveKitRoomOptions is a convenience builder that snapshots LiveKit
signaling, credentials, region selection and encryption settings into RoomOptions.
Custom transports can use a different configuration without LiveKit credentials.
Room snapshots expose BackendCapabilities; acknowledged participant updates are
gated by adapter capability rather than a protocol version in RoomEngine.

Only LiveKit is implemented. The mediasoup directory remains an extension note.
Single-PC negotiation, seamless resume/migration and simulcast/SVC are not added
by this refactor. Windows automated results and physical-device acceptance must
be reported separately; this document is not a device-acceptance claim.

## Migration

1. Replace LiveKit connection builders named RoomOptions with LiveKitRoomOptions
   and include `livekit/rtc/livekit.hpp`. RoomTransport overrides still accept
   the neutral RoomOptions.
2. Include `rtc/graph_api.hpp` for custom GraphNodes; change advanced calls to
   `engine.graph().subscribe/publish_graph/subscribe_graph`. Plain encoded
   `engine.publish(descriptor)` remains on the ordinary facade.
3. Remove all RtcEngine/pipeline poll calls and receive-frame poll budgets.
   Futures now progress autonomously. Use RtcExecutor only when an application
   needs an additional bounded command/future adapter. Rename graph `poll` hooks
   to `check_health`; these hooks must only check health. Replace engine event/failure
   reads with `set_callbacks`, or `RoomEngine::set_event_handler` for backend-only use.
4. Media callbacks now run on per-output dispatchers. Retaining immutable frames
   is safe; callbacks must not wait for lifecycle completion.

See `examples/automatic_session` for a headless callback-based autonomous publication.
