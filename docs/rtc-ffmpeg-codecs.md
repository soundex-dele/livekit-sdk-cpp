# Optional H.264/H.265 RTC codecs

`LIVEKIT_RTC_FFMPEG_CODECS=ON` adds software H.264 and H.265 encoder/decoder factories to the native C++ SDK. It is off by default. The implementation uses libavcodec directly, retains the SDK's media graph, and sends compressed video through the normal WebRTC RTP path. It does not launch FFmpeg processes for RTC encoding.

The current backend supports one layer, 8-bit I420, even dimensions up to 1280×720 and up to 30 fps. H.264 advertises constrained baseline, level 3.1, packetization-mode 1. H.265 advertises Main, level 3.1, SRST. It disables B frames and delayed encoding, repeats parameter sets at keyframes, and handles WebRTC keyframe requests with IDR pictures. A zero bitrate pauses encoding. Material bitrate changes reopen the encoder and produce a new IDR; this software path is not a hardware encoder implementation.

Set the `MediaSpec.fps` to the source's intended maximum. The SDK now propagates it to `RtpEncodingParameters.max_framerate`, including when publications are recreated. This prevents native WebRTC defaults from requesting 60 fps for a 30 fps source.

Supply a matching FFmpeg shared runtime, headers, and import libraries containing `libx264`, `libx265`, H.264 and HEVC decoders. Formats are advertised only when the corresponding implementation exists. Encoder availability alone does not establish that a particular third-party binary works: run the codec test before interoperability tests.

Example for the isolated dependency used by the UAV tests:

```powershell
cmake -S livekit-sdk-cpp -B livekit-sdk-cpp/out/windows-x64 `
  -U 'LIVEKIT_FFMPEG_*_LIBRARY' `
  -DLIVEKIT_RTC_FFMPEG_CODECS=ON `
  -DLIVEKIT_FFMPEG_ROOT=D:/workspace/av-platform/out/uav-codec-deps/ffmpeg-n8.1-latest-win64-gpl-shared-8.1 `
  -DLIVEKIT_FFMPEG_INCLUDE=D:/workspace/av-platform/out/uav-codec-deps/ffmpeg-n8.1-latest-win64-gpl-shared-8.1/include
cmake --build livekit-sdk-cpp/out/windows-x64 --config Release `
  --target livekit_uav_requirements livekit_ffmpeg_codec_tests --parallel 4
ctest --test-dir livekit-sdk-cpp/out/windows-x64 -C Release `
  -R '^ffmpeg_rtc_codecs$' --output-on-failure
```

The UAV test and codec test copy runtime DLLs beside their executables on Windows. Other consumers must deploy matching DLLs or arrange their runtime library search path. FFmpeg libraries are optional dependencies and are not checked into this repository. The tested package is a GPL build with x264/x265; distribution must retain its licensing requirements and notices.

Dependency used for the local validation:

- Asset: `ffmpeg-n8.1-latest-win64-gpl-shared-8.1.zip` from the BtbN FFmpeg-Builds `latest` release, retrieved 2026-10-27.
- Archived locally as `out/uav-codec-deps/ffmpeg-n8.1-win64-gpl-shared.zip`.
- Archive SHA256: `88cda345c479dd3e18b8e33fa4835d350b1084b1f8008617a396531b05f5c851`.
- The moving `latest` URL is not a reproducible pin; preserve/check this archive hash or validate a replacement independently.

This adds codec transport capability. It does **not** implement LiveKit backup-codec REGRESSION signaling, first-viewer capability decisions, video FEC, target ARM64 hardware encoding, or an automatic H.265→H.264 application policy.
