# Source-driven RTC runtime

This supersedes the manual-poll execution contract in the earlier architecture
refactor record. RtcEngine, RtcPipelineEngine and per-track pipeline poll APIs
are removed, without compatibility wrappers.

- Control: synchronous Result on application workers, serialized by one room operation lock.
  No engine control TaskQueue, executor adapter or public completion callbacks.
- Room state: existing jthread, bounded command/event mailbox and deadline index.
  Private completion records wake condition-variable waiters; background paths never take the operation lock.
- Notifications: existing serial TaskQueue; controls in callbacks fail before locking.
  Replacing/removing callbacks fences old invocations. Callbacks must return promptly.
- Data: subscription nodes block on bounded endpoint queues and publish on source workers.
- Feedback: publication workers wait for revision/epoch changes and deliver initial feedback before activation.
- Failure: nodes actively report errors. The next control operation reaps failed graphs;
  no delayed health polling, public pending media handle or control queue remains.
- Lifecycle: leave/close wait for preceding operations, then join producers, stop graphs,
  wait for detachment and fence notifications. Protocol timeout excludes lock waiting.

The current verification record is [RTC synchronous Result validation](rtc-sync-result-validation-20261029.md).
The results below describe the earlier runtime migration, not this refactor's acceptance.

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
