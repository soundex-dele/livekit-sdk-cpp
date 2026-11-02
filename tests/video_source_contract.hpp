#pragma once
#include "livekit/compat/thread.hpp"
#include "media_result.hpp"
#include "media_nodes/capture/video_source.hpp"
#include <future>
#include <thread>
#include "test_source_location.hpp"
#include <iostream>

inline void video_source_contract() {
  using namespace livekit::sdk;
  using namespace std::chrono_literals;
  auto require = [](bool ok, const livekit::sdk::test::SourceLocation location =
                                 livekit::sdk::test::SourceLocation::current()) {
    if (!ok) {
      std::cerr << "I420 source contract failed at line " + std::to_string(location.line()) << "\n";
      std::abort();
    }
  };
  auto source = std::make_shared<I420VideoSource>(I420SourceOptions{4, 4, 2, 48, 20ms});
  VideoFrame padded;
  padded.width = padded.height = 4;
  padded.stride_y = 6;
  padded.stride_u = padded.stride_v = 3;
  padded.y = {1, 2, 3, 4, 99, 99, 5, 6, 7, 8, 99, 99, 9, 10, 11, 12, 99, 99, 13, 14, 15, 16};
  padded.u = {70, 71, 99, 72, 73};
  padded.v = {80, 81, 99, 82, 83};
  require(media_result(source->write(padded)) == VideoWriteStatus::backpressure);
  {
    detail::I420SourceReader reader(source);
    detail::I420SourceReader duplicate(source);
    bool refused = !duplicate.status();
    require(refused && source->stats().attached);
    media_result(reader.start());
    require(media_result(source->write(padded)) == VideoWriteStatus::accepted);
    auto owned = reader.read({});
    require(owned &&
            owned->frame.y ==
                std::vector<uint8_t>({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}) &&
            owned->frame.u == std::vector<uint8_t>({70, 71, 72, 73}) && owned->frame.stride_y == 4);
    padded.y[0] = 42;
    require(owned->frame.y[0] == 1);
    require(media_result(source->write(padded)) == VideoWriteStatus::accepted &&
            media_result(source->write(padded)) == VideoWriteStatus::accepted);
    require(media_result(source->write(padded)) == VideoWriteStatus::backpressure &&
            source->stats().queued_bytes == 48);
    std::this_thread::sleep_for(30ms);
    require(media_result(source->write(padded)) == VideoWriteStatus::accepted &&
            source->stats().dropped == 2);
    livekit::compat::stop_source cancel;
    cancel.request_stop();
    require(!reader.read(cancel.get_token()) && source->stats().queued_frames == 1);
    reader.stop();
    require(source->stats().queued_frames == 0 && !source->stats().accepting);
    media_result(reader.start());
    require(source->stats().generation == 2);
    auto waiting = std::async(std::launch::async, [&] { return reader.read({}); });
    reader.stop();
    require(waiting.wait_for(1s) == std::future_status::ready && !waiting.get());
    media_result(reader.start());
    auto cancelled = std::async(std::launch::async, [&] {
      livekit::compat::stop_source local;
      local.request_stop();
      return reader.read(local.get_token());
    });
    require(cancelled.wait_for(1s) == std::future_status::ready && !cancelled.get());
    auto bad = padded;
    bad.stride_y = -1;
    refused = !source->write(bad);
    require(refused);
    bad = padded;
    bad.v.pop_back();
    refused = !source->write(bad);
    require(refused);
  }
  require(!source->stats().attached &&
          media_result(source->write(padded)) == VideoWriteStatus::backpressure);
  {
    detail::I420SourceReader rebound(source);
    media_result(rebound.start());
    require(source->stats().generation == 4 &&
            media_result(source->write(padded)) == VideoWriteStatus::accepted);
    auto retained = rebound.read({});
    auto waiting = std::async(std::launch::async, [&] { return rebound.read({}); });
    source->close();
    require(waiting.wait_for(1s) == std::future_status::ready && !waiting.get());
    require(retained && retained->frame.y[0] == 42 &&
            media_result(source->write(padded)) == VideoWriteStatus::closed);
  }
  detail::I420SourceReader closed(source);
  bool refused = !closed.status();
  require(refused);
  auto bytes = std::make_shared<I420VideoSource>(I420SourceOptions{4, 4, 100, 24, 1s});
  detail::I420SourceReader byte_reader(bytes);
  media_result(byte_reader.start());
  require(media_result(bytes->write(padded)) == VideoWriteStatus::accepted &&
          media_result(bytes->write(padded)) == VideoWriteStatus::backpressure);
  auto timed = std::make_shared<I420VideoSource>(I420SourceOptions{4, 4, 3, 72, 1s});
  detail::I420SourceReader timed_reader(timed);
  media_result(timed_reader.start());
  const auto captured = std::chrono::steady_clock::now() - 100ms;
  require(media_result(timed->write(padded, captured)) == VideoWriteStatus::accepted);
  require(timed_reader.read({})->captured == captured);
  auto rejected = [&](auto fn) {
    auto result = fn();
    require(!result && result.error().code == ErrorCode::invalid_argument);
  };
  rejected([&] { return timed->write(padded, captured); });
  rejected([&] { return timed->write(padded, std::chrono::steady_clock::now() + 1s); });
  timed_reader.stop();
  media_result(timed_reader.start());
  require(media_result(timed->write(padded, std::chrono::steady_clock::now() - 2s)) ==
          VideoWriteStatus::backpressure);
  require(media_result(timed->write(padded, captured)) == VideoWriteStatus::accepted);
  timed_reader.read({});
  RgbaInputFrame rgba;
  rgba.width = rgba.height = 4;
  rgba.stride = 20;
  rgba.pixels.resize(76, 99);
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) {
      const auto p = y * 20 + x * 4;
      rgba.pixels[p] = 255;
      rgba.pixels[p + 1] = rgba.pixels[p + 2] = 0;
      rgba.pixels[p + 3] = 255;
    }
  require(media_result(timed->write_rgba(rgba)) == VideoWriteStatus::accepted);
  auto red = timed_reader.read({});
  std::cerr << "RGBA red YUV=" << int(red->frame.y[0]) << '/' << int(red->frame.u[0]) << '/'
            << int(red->frame.v[0]) << '\n';
  // Pinned libyuv's integer BT.601 conversion gives 82/90/239 for RGBA red.
  require(red->frame.y[0] == 82 && red->frame.u[0] == 90 && red->frame.v[0] == 239);
  // Distinct 2x2 quadrants make rotation/chroma direction observable.
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) {
      const auto p = y * 20 + x * 4;
      const auto white = x < 2 && y < 2;
      rgba.pixels[p] = rgba.pixels[p + 1] = rgba.pixels[p + 2] = white ? 255 : 0;
    }
  for (auto rotation : {VideoRotation::none, VideoRotation::clockwise90,
                        VideoRotation::clockwise180, VideoRotation::clockwise270}) {
    rgba.rotation = rotation;
    require(media_result(timed->write_rgba(rgba)) == VideoWriteStatus::accepted);
    auto rotated = timed_reader.read({});
    const auto corner = rotation == VideoRotation::none           ? 0
                        : rotation == VideoRotation::clockwise90  ? 3
                        : rotation == VideoRotation::clockwise180 ? 15
                                                                  : 12;
    require(rotated->frame.y[corner] == 235);
    require(std::count(rotated->frame.y.begin(), rotated->frame.y.end(), 235) == 4);
  }
  rgba.rotation = static_cast<VideoRotation>(45);
  rejected([&] { return timed->write_rgba(rgba); });
  rgba.rotation = VideoRotation::none;
  rgba.pixels.pop_back();
  rejected([&] { return timed->write_rgba(rgba); });
  auto rectangular = std::make_shared<I420VideoSource>(I420SourceOptions{4, 2, 1, 12, 1s});
  detail::I420SourceReader rectangular_reader(rectangular);
  media_result(rectangular_reader.start());
  RgbaInputFrame portrait{2, 4, 8, std::vector<uint8_t>(32, 255), VideoRotation::clockwise90};
  require(media_result(rectangular->write_rgba(portrait)) == VideoWriteStatus::accepted);
  auto landscape = rectangular_reader.read({});
  require(landscape->frame.width == 4 && landscape->frame.height == 2);
}
