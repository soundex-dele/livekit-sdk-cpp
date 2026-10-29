# Runtime convergence work

Scope: the six issues in the SDK architecture review. Implementation and caller
migration precede unified build/runtime verification. Public API changes are
allowed; source-node-owned media delivery remains an invariant.

Required outcomes:

- Backend events and writable/queued media wake the room controller. Deadlines
  and cancellation have explicit wakeups; remove periodic transport polling and
  track/future readiness scans from the room controller.
- Nodes actively report failures and terminal endpoint transitions. Remove the
  engine's periodic graph health scan, including its custom graph hook.
- RoomServices business notifications use callbacks. Player output has explicit
  callback delivery and device demand semantics; migrate all repository callers.
- Engine operations enqueue and return a completion result without waiting for
  control execution. Continuations replace future-readiness timers.
- Bound and expose thread/queue resources, share suitable dispatch resources,
  retain dedicated source delivery and isolation of slow media consumers, and
  verify multi-track behavior under load.
- Use structured, correlated failure information for operation results, room
  events, graph callbacks and track state; specify delivery and deduplication.

Implementation status (source changes; not yet built or run):

- Added AsyncResult / CompletionSource continuations, asynchronous engine
  admission, individual pipeline completion callbacks and shared executor/control
  dispatch. Known engine-result callers are migrated; final repository-wide audit
  remains outstanding.
- Room controller now processes coalesced work for exact track incarnations.
  A bounded ReadySet drops notifications from retired identities. DeadlineIndex
  retains one live timeout per operation/track, erasing updates/cancellations
  immediately. Removed per-wakeup track/control scans and snapshot counting scans;
  whole-registry walks remain only for session interruption/recovery/shutdown.
- Native take_events drains only notified sender/receiver bindings. A sender
  returning would_block now explicitly reports writable for that binding when
  capacity returns. Receiver replacement shares its current binding wake handle.
  Room snapshots expose serviced-track/wakeup counters and pending deadlines/work.
  Added unrun scheduling churn/stale-wakeup and 128-track sparse-activity tests.
- Native SDP/statistics operations now return AsyncResult and deliver completed
  results through a bounded session completion queue. Added native connection,
  ICE, track, data-channel state/capacity/arrival wakeups. SignalSocket connect,
  initial join and resume waits now use actual deadlines and stop notification.
  The main native loop still has a short receive wait until service producers,
  service deadlines and binding-change work are fully integrated.
- RPC no longer wakes every 10 ms or scans all active requests. Its deadline
  worker waits on an indexed earliest deadline and a bounded cancellation ready
  set, processing only due/cancelled request identities. ACK updates, completed
  response cache expiry, participant departure and disconnect remove/replace
  index entries immediately. Async responses and packet admission now wake the
  native session. Added internal scheduling diagnostics and unrun sparse-cancel,
  deadline replacement, earlier-deadline and retired-cancellation tests.
- Plain RoomServices sends now index individual deadlines/cancellations, including
  requests behind a backpressured FIFO head. Admission wakes the native session;
  bounded successful send quanta explicitly reschedule remaining work, while a
  blocked sender waits for capacity, cancellation or expiry. Added queued-send
  diagnostics and unrun blocked-head cancellation/timeout and bounded-drain tests.
  Stream/DataTrack producers and deadlines still need the same integration, and
  native waiting does not yet consume the new plain-send deadline accessor.
- Removed the engine health timer and TrackGraphSpec.check_health. Graph nodes
  report faults actively; source workers report endpoint terminal states. Owner
  cleanup compares endpoint identity to fence callbacks from replaced graphs.
- Codec, input, recording, application output, shared capture, microphone and
  audio-output device faults now notify their graphs. Shared devices use retained
  failure reporters rather than borrowed node pointers. TrackGraphSpec.on_failure
  updates retained media-track state before teardown.
- Added correlated FailureInfo to graph notifications, room failure events and
  endpoint/media snapshots. Startup/runtime graph failures are routed back to the
  room. Remaining operation/service/callback errors still need a full audit.
- Failed-graph cleanup now participates in the native detachment barrier.
  Detachment completions use a bounded-by-outstanding-removals mailbox rather
  than competing for user command admission. Added a deferred-detachment case.
- Migrated the three fixture transports to take_events and wake notifications.
  Replaced health-hook integration fixtures. Added source/graph failure, stale
  incarnation, idle shared-device, output-callback and retained-reporter tests.
- RoomServices now delivers data callbacks on the room notification queue, with
  in-flight fences, self-removal and correlated callback failures. CLI/remote
  control use application-owned bounded mailboxes; conference chat consumes
  callbacks directly. Added delivery/fence/failure tests (not run).
- Player now has callback outputs and bounded audio-demand credits. Paused seek
  previews, terminal drain and lifecycle fences no longer depend on public media
  polling. Fixed self-replacement fences and callback destruction under locks.
  Desktop playback uses a bounded frame mailbox and Qt-posted control completions;
  Studio uses bounded media mailboxes and asynchronous open/replay completions.
  Migrated Player tests and internal output reads, and added device-credit,
  pause/close/removal-fence and failure-correlation tests (not run).
- Migrated remaining known RTC track-result containers and fixture wait helpers
  to AsyncResult; native receiver fixtures now use take_frames. Repository-wide
  callback/lifetime audit and resource budgets/diagnostics are still outstanding.
- Player currently uses one serial output lane for audio/video/state; source and
  scene graph scheduling still need convergence with the source-owned runtime.
  Native main-loop integration, service deadlines/wakeups and session binding
  readiness scans remain open.

No builds or tests have been run for this work. All verification remains deferred
until the full implementation and caller migrations are complete.

Final gates: affected SDK and product builds; unit and headless integration
tests; race/cancellation/destruction and multi-track resource tests; isolated SFU
scenarios; API/caller scan; diff checks. Physical-device and cross-platform manual
acceptance remain separately reported.
