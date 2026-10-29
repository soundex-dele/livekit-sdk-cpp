# SDK architecture refactor

Execution model update: manual engine/pipeline polling below is historical. See
[source-driven runtime](source-driven-runtime.md) for the subsequent task-queue
redesign and its separate validation record.

Scope: implement the architecture review accepted on 2026-10-29. Public C++ API
changes are allowed; migrate repository consumers together. Preserve GraphEngine
as the media composition mechanism. No desktop automation.

## Delivery and evidence

- [x] Separate ordinary RtcEngine API from advanced graph API. Remove upward
  dependencies from codec/input/output targets. Prove ordinary headers compile
  without GraphFlow include paths and rebuild affected consumers.
- [x] Unify shared frame ownership/time/format contracts across RTC and playback
  adapters; remove per-consumer raw-frame copies from shared capture. Verify buffer
  identity, independent queue limits, cancellation and cross-room lifetime.
- [x] Provide automatic owner-executor driving alongside manual polling, bounded
  callback delivery and scheduling diagnostics. Test progress without UI polling,
  shutdown/cancellation/rejoin and slow-consumer isolation.
- [x] Implement receive audio jitter/PLC, capture APM/AEC reference integration,
  and cross-track media timing/synchronization. Test loss, jitter, discontinuity,
  clock drift, echo reference lifetime and device-independent processing. Physical
  device acceptance remains separately reported.
- [x] Separate backend-specific configuration and capabilities from common room
  contracts, without claiming an unimplemented mediasoup transport.
- [x] Update current architecture/API documentation and migration examples. Run
  relevant SDK and application builds/tests plus isolated SFU integration; record
  platform and actual-device limitations explicitly.

## Implementation and validation

Implemented all six items above, then performed unified validation as requested.
Public API changes and repository caller migrations are intentional. No commits
or pushes were made. Architecture and migration contracts are in [architecture.md](architecture.md).

The final review also fixed independent atomic-counter snapshot underflow,
preserved the source PCM timestamp through shared capture fanout, propagated
capture generation boundaries to APM, and made old graph users link their
GraphEngine dependency explicitly. Invalid shared queue configuration is rejected
synchronously without disturbing active branches.

### Windows automated evidence (2026-10-29)

Paths below are relative to the aggregate av-platform workspace. All commands
completed successfully unless explicitly described as unavailable below.

| Gate | Result | Evidence |
| --- | --- | --- |
| SDK full Release build, examples and integration binaries | Passed | `out/architecture-sdk-build-final2.log` |
| SDK CTest | 23/23 passed | `out/architecture-sdk-tests-final.log` |
| Application full Release build, including Player/Studio and desktop products | Passed | `out/architecture-app-build-final2.log` |
| Application CTest, `QT_QPA_PLATFORM=offscreen` | 21/21 passed | `out/architecture-app-tests-final.log` |
| Native/backend-disabled configuration build | Passed | `out/architecture-backend-build.log` |
| Pure backend/pipeline CTest | 2/2 passed | `livekit-sdk-cpp/out/rtc-backend-contracts/Testing/Temporary/LastTest.log` |
| Ordinary engine header without GraphFlow include paths | Compile-only target passed | `tests/public_engine_header.cpp`, target `livekit_public_engine_header` |
| New native source format | 17 files passed clang-format 20.1.8 | Non-empty formatter output compared against source bytes |
| Whitespace/diff checks | Passed in SDK and app repositories | `git diff --check` |

`rtc_architecture` checks automatic polling, shutdown/cancellation/rejoin, immutable
shared buffer identity and branch lifetime, RTC/Player owning frame views,
sender-time mapping, audio-master drift, stale epochs, slow callback isolation,
actual fake-speaker mixed reference, APM format/lifetime and deterministic delayed
AEC convergence (late output energy below 30% of input). `rtc_audio_nodes` checks
Opus/NetEq loss, reordering, sequence wrap and epoch reset. Existing media-engine
tests cover shared input across rooms and independent branch close.

### Isolated SFU evidence

Each run used `tests/rtc_livekit_integration.py` with the explicitly supplied local
server `out/uav-sfu-go/livekit-server.exe`. Generated media and fake audio output
avoid opening physical devices. Result paths are under
`out/rtc-livekit-integration/`:

| Client/scenario | Result file |
| --- | --- |
| Ordinary API + RtcExecutor + NetEq 10 ms PCM + sender clock + shared A/V presentation clock + frames retained after shutdown | `20261029-132448-0k60z_bi/result.json` |
| Encoded multi-graph transmission, independent close, leave/rejoin, service isolation | `20261029-132024-3n3xn3cy/result.json` |
| Raw audio, 44.1 kHz resampling, stereo, shared mixed output, independent close | `20261029-132451-0v_7tz9k/result.json` |
| Same audio graph with media E2EE | `20261029-132455-5l9r64_9/result.json` |
| Raw video graphs, independent close and mute recovery | `20261029-132457-t_5et7gk/result.json` |

The new ordinary-media client is `tests/rtc_media_integration.cpp`; it includes
only ordinary SDK headers and verifies received sender timing and presentation
offsets through the actual SFU, rather than inferring synchronization from unit
tests or encoded packet delivery.

### Limits and unsuccessful optional check

- The optional encrypted-video run failed at encoder creation with
  `Requested video encoder unavailable` because this SDK build has
  `LIVEKIT_RTC_FFMPEG_CODECS=OFF` and no H.264 encoder. Evidence:
  `out/rtc-livekit-integration/20261029-132106-082gpq16/client.stderr.log`.
  H.264 encrypted video is **not validated** by this run; VP8 encryption is not
  supported by the existing transport. Plain VP8 video and encrypted Opus passed.
- These are Windows builds, synthetic/fake-device tests, isolated SFU tests and
  offscreen render checks. No actual camera, microphone or speaker was manually
  accepted; macOS/Linux were not built or tested in this session.
- AEC convergence is a deterministic synthetic echo-path check, not acoustic-room
  acceptance. Default echo reference is scoped to one engine output; there is no
  global cross-room physical-device manager.
- mediasoup, simulcast/SVC and seamless signal resume remain outside this refactor.
