#pragma once
#include "media_nodes/capture/video_source.hpp"
#include <future>
#include <thread>

inline void video_source_contract() {
  using namespace livekit::sdk;
  using namespace std::chrono_literals;
  auto require = [](bool ok) { if (!ok) throw std::runtime_error("I420 source contract failed"); };
  auto source = std::make_shared<I420VideoSource>(I420SourceOptions{4, 4, 2, 48, 20ms});
  VideoFrame padded; padded.width = padded.height = 4;
  padded.stride_y = 6; padded.stride_u = padded.stride_v = 3;
  padded.y = {1,2,3,4,99,99,5,6,7,8,99,99,9,10,11,12,99,99,13,14,15,16};
  padded.u = {70,71,99,72,73}; padded.v = {80,81,99,82,83};
  require(source->write(padded) == VideoWriteStatus::backpressure);
  {
    detail::I420SourceReader reader(source);
    bool refused = false;
    try { detail::I420SourceReader duplicate(source); } catch (const std::invalid_argument&) { refused = true; }
    require(refused && source->stats().attached);
    reader.start();
    require(source->write(padded) == VideoWriteStatus::accepted);
    auto owned = reader.read({});
    require(owned && owned->frame.y == std::vector<uint8_t>({1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}) &&
        owned->frame.u == std::vector<uint8_t>({70,71,72,73}) && owned->frame.stride_y == 4);
    padded.y[0] = 42; require(owned->frame.y[0] == 1);
    require(source->write(padded) == VideoWriteStatus::accepted && source->write(padded) == VideoWriteStatus::accepted);
    require(source->write(padded) == VideoWriteStatus::backpressure && source->stats().queued_bytes == 48);
    std::this_thread::sleep_for(30ms);
    require(source->write(padded) == VideoWriteStatus::accepted && source->stats().dropped == 2);
    std::stop_source cancel; cancel.request_stop();
    require(!reader.read(cancel.get_token()) && source->stats().queued_frames == 1);
    reader.stop(); require(source->stats().queued_frames == 0 && !source->stats().accepting);
    reader.start(); require(source->stats().generation == 2);
    auto waiting = std::async(std::launch::async, [&] { return reader.read({}); });
    reader.stop(); require(waiting.wait_for(1s) == std::future_status::ready && !waiting.get());
    reader.start();
    auto cancelled = std::async(std::launch::async, [&] { std::stop_source local; local.request_stop(); return reader.read(local.get_token()); });
    require(cancelled.wait_for(1s) == std::future_status::ready && !cancelled.get());
    auto bad = padded; bad.stride_y = -1;
    refused = false; try { source->write(bad); } catch (const std::invalid_argument&) { refused = true; } require(refused);
    bad = padded; bad.v.pop_back();
    refused = false; try { source->write(bad); } catch (const std::invalid_argument&) { refused = true; } require(refused);
  }
  require(!source->stats().attached && source->write(padded) == VideoWriteStatus::backpressure);
  {
    detail::I420SourceReader rebound(source); rebound.start();
    require(source->stats().generation == 4 && source->write(padded) == VideoWriteStatus::accepted);
    auto retained = rebound.read({});
    auto waiting = std::async(std::launch::async, [&] { return rebound.read({}); });
    source->close();
    require(waiting.wait_for(1s) == std::future_status::ready && !waiting.get());
    require(retained && retained->frame.y[0] == 42 && source->write(padded) == VideoWriteStatus::closed);
  }
  bool refused = false;
  try { detail::I420SourceReader closed(source); } catch (const std::invalid_argument&) { refused = true; } require(refused);
  auto bytes = std::make_shared<I420VideoSource>(I420SourceOptions{4,4,100,24,1s});
  detail::I420SourceReader byte_reader(bytes); byte_reader.start();
  require(bytes->write(padded) == VideoWriteStatus::accepted && bytes->write(padded) == VideoWriteStatus::backpressure);
}
