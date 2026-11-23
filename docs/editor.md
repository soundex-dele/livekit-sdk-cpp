# Independent editor engine

Enable `LIVEKIT_BUILD_EDITOR=ON` and link `LiveKit::Editor`. The public header is
`<livekit/editor.hpp>`; it exposes C++17 values and explicit `Result` / `AsyncResult`
operations without Qt, FFmpeg, GraphFlow or RTC types. FFmpeg discovery and runtime
deployment share `cmake/PlayerFFmpeg.cmake` with the local player.

`EditorProject` owns assets, ordered tracks and clips. Positions, source in-points
and durations are integer project frames with a rational `EditorRate`. Clip ranges
are half-open. Track order is bottom to top; same-track overlaps are rejected.
`editor_validate` checks IDs, ranges, resource limits and properties. Asset paths
are UTF-8 local files. `EditorDocument` is confined to its application owner thread;
it validates before committing and keeps up to 100 reversible project snapshots.
Locked tracks reject clip changes, including moves to another track.

`editor_save` writes version 1 JSON with relative media references where possible,
then atomically replaces the destination. `editor_load` retains missing media and
rejects malformed versions/types/ranges. Relinking replaces an asset while keeping
its ID; validation rejects replacements too short for existing clips. Missing media
is reported when an affected clip is rendered. Files are not embedded in projects.

`EditorSession` owns a worker and a typed inline GraphFlow graph. The timeline sends active clip work
through decode, composition, mixing and output nodes using synchronous typed graph
edges on that worker. FFmpeg demuxers and decoders are local-file adapters; they are
not exposed to applications. Decoder entries use LRU eviction (up to eight stream
decoders, estimated decoded-image cache budget 256 MiB). Decoder threads are limited
to two per stream. Input and output images are limited to 4096×4096.

Color correction runs in a per-clip inline `graphflow::Graph` from `graphflow-cpp`.
The grade node applies an exposure/temperature lookup table; the saturation/contrast node
consumes its quantized RGB values. Both operate on one private RGBA copy, preserving alpha
and the decoded input. A project edit with unchanged color settings reuses that clip's
graph. Time stretch uses the graph-independent phase vocoder; transitions join the two
layer transactions before processing RGBA. There is no EventBus or legacy graph fallback.
Project updates start a complete candidate graph on the render worker and only then
replace the active graph. Unchanged processor state and decoder caches are retained;
no emitter, node binding or active callback is transferred between graphs.

Controls enqueue at most 64 commands. Pending seeks coalesce, with cancelled results
for superseded requests. Replacing a project pauses playback; a generation tags all
frames so an application can discard earlier project output. Seek also pauses and
delivers a target frame. Audio is interleaved stereo float at 48 kHz; each output
contains the exact sample interval for its project frame. Playback uses one steady
clock and never independently advances audio or video. Applications flush their
device buffers on pause, seek and project replacement.

Set callbacks at construction. Callbacks run on the session worker, must return
promptly, and must not wait for session operations or destroy the session.
Destruction cancels input, joins the worker and fences all callbacks. The application
must stop submitting work before destruction. The Qt consumer bounds its delivery
mailbox to eight frames / 128 MiB and pauses on overflow.

`EditorExport` owns an independent worker and immutable project snapshot. It runs
the same render graph with an offline clock, encodes software H.264 / AAC MP4, and
reports progress. An instance accepts one export at a time; reuse is allowed after
completion. Dispatch completion continuations before restarting an export, and
never destroy it from its worker callbacks. `cancel()` is thread-safe. Cancellation
or a failure removes only the temporary file; successful finalization atomically
replaces the destination. Export rejects a destination that aliases a source asset.
Even video-only and audio-only timelines produce both streams, using silence or a
black canvas as appropriate.

Enable `LIVEKIT_EDITOR_BUILD_TESTS` for model and synthetic media tests. Tests create
fixtures with the selected FFmpeg executable and inspect actual encoded outputs.
They do not open sound or display devices.
