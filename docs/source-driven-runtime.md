# Source-driven RTC runtime

This supersedes the manual-poll execution contract in the earlier architecture
refactor record. RtcEngine, RtcPipelineEngine and per-track pipeline poll APIs
are removed, without compatibility wrappers.

- Control: owned utoolkit TaskQueue, bounded API admission, backend completion
  notifications, serialized graph construction/teardown, automatic futures.
- Notifications: RoomEngine::set_event_handler and RtcEngine::set_callbacks;
  no engine next_event/next_failure APIs. Serial notification TaskQueue is separate
  from control/media. Replacement/removal fences old invocations; self-removal
  invalidates queued old graph failures. Callbacks may call control methods,
  must return promptly, must not destroy the engine, and must not throw.
  Throwing callbacks are disabled and fail the room. Register before join and
  serialize registration calls. Borrowed event arguments last only for the call.
- Applications/tests with existing session loops use a non-installed bounded
  mailbox in support/; it receives callbacks and owns its event copies.
- Data: subscription node worker blocks on the existing bounded endpoint queue;
  publishes synchronously into GraphEngine on the source thread.
- Feedback: publication node worker waits for revision/epoch changes; initial
  feedback is delivered before producer activation.
- Lifecycle: stop/cancel/join source workers before stopping downstream nodes;
  errors cross thread boundaries as stored exceptions and graph failure records.
- Monitoring: delayed health-only tasks, no timer-driven media or graph creation.
- Optional RtcExecutor: task-queue command adapter with command metrics; no engine
  polling. Its generic std::future adapter checks readiness only while pending.

Custom GraphFlow nodes must record asynchronous processing errors and expose
them through check_health; GraphFlow EventBus catches raw subscriber exceptions.

The task_queue dependency is usable without the full GraphFlow build. Its
Windows macros are target-local, duplicate addition is guarded, and Event::set /
reset now retain the mutex while modifying the wakeup state.

Validation completed (Windows Release, 2026-10-29):
- Backend callback and 2,000-round event wakeup contracts passed.
- Pipeline contracts passed 30 consecutive repetitions. An earlier run timed out
  during concurrent builds; after adding stage diagnostics it did not recur.
- Five isolated SFU scenarios passed: engine multi-graph without media poll,
  ordinary media with sender clock/A/V presentation and retained frames,
  audio graphs, encrypted audio graphs, and video graphs with mute recovery.
- Full SDK build and CTest: 23/23 passed (`out/source-runtime-sdk-tests.log`).
- Full application build and offscreen CTest: 21/21 passed
  (`out/source-runtime-app-tests.log`).
- Separate backend build and final CTest: 2/2 passed
  (`out/source-runtime-backend-tests.log`).
- Audio scenarios were rerun after final linking, both passed. H.264 encrypted
  video remains unverified because this build disables FFmpeg RTC codecs.
- SDK, application and nested utoolkit diffs pass `git diff --check`.
Log paths above are relative to the aggregate av-platform workspace.
All validation uses builds, automated tests and isolated/headless integration.
Physical devices and macOS/Linux remain separate acceptance work.
