#pragma once
#include "media_nodes/render/audio_stream.hpp"
#include <future>

inline void audio_stream_contract() {
  using namespace livekit::sdk;
  using namespace std::chrono_literals;
  auto require = [](bool ok) { if (!ok) throw std::runtime_error("Audio stream contract failed"); };
  auto stream = std::make_shared<AudioFrameStream>(AudioStreamOptions{3, 3840});
  detail::AudioFrameOutput output(stream); output.advance();
  auto a = output.add("a"), b = output.add("b");
  std::vector<int16_t> negative(960, -16384), positive(960, 16384);
  output.submit("a", a, negative.data(), 16, 48000, 2, 480, 123);
  output.submit("b", b, positive.data(), 16, 48000, 2, 480, {});
  output.submit("a", a, negative.data(), 16, 48000, 2, 480, 133);
  require(stream->stats().queued_frames == 2 && stream->stats().queued_bytes == 3840 && stream->stats().dropped_samples == 480);
  auto frame_a = stream->read_for("a", 0ms);
  auto frame_b = stream->read_for("b", 0ms);
  require(frame_a.status == AudioReadStatus::frame && frame_b.status == AudioReadStatus::frame);
  require(frame_a.frame->sequence == 1 && frame_a.frame->sample_index == 480 && frame_a.frame->samples.front() == -16384);
  require(frame_b.frame->sequence == 0 && frame_b.frame->sample_index == 0 && frame_b.frame->samples.front() == 16384);
  require(frame_a.frame->duration() == 10ms && frame_a.frame->capture_timestamp_ms == 133 && !frame_b.frame->capture_timestamp_ms);
  output.submit("a", a, negative.data(), 16, 24000, 1, 240, {});
  auto changed = stream->read_for("a", 0ms).frame;
  require(changed && changed->format_changed && changed->track_generation != frame_a.frame->track_generation && changed->sample_index == 0);
  output.remove("a", a); auto replacement = output.add("a");
  output.submit("a", a, negative.data(), 16, 48000, 2, 480, {});
  require(stream->read(0ms).status == AudioReadStatus::timeout); // late old sink
  output.submit("a", replacement, negative.data(), 16, 48000, 2, 480, {});
  output.advance(); require(stream->read(0ms).status == AudioReadStatus::timeout);
  output.submit("a", replacement, negative.data(), 16, 48000, 2, 480, {});
  auto renewed = stream->read(0ms).frame;
  require(renewed && renewed->stream_generation == 2 && renewed->sample_index == 0);
  auto waiting = std::async(std::launch::async, [&] { return stream->read_for("b", 2s); });
  output.submit("a", replacement, negative.data(), 16, 48000, 2, 480, {});
  require(waiting.wait_for(20ms) == std::future_status::timeout);
  output.submit("b", b, positive.data(), 16, 48000, 2, 480, {});
  require(waiting.wait_for(1s) == std::future_status::ready && waiting.get().frame->track_id == "b");
  std::stop_source stop; stop.request_stop();
  require(stream->read(0ms, stop.get_token()).status == AudioReadStatus::cancelled && stream->stats().queued_frames == 1);
  output.remove("a", replacement); require(stream->stats().queued_frames == 0);
  auto closing = std::async(std::launch::async, [&] { return stream->read(10s); });
  output.close();
  require(closing.wait_for(1s) == std::future_status::ready && closing.get().status == AudioReadStatus::closed);
  require(stream->stats().tracks == 0 && frame_a.frame->samples.front() == -16384);
  bool refused = false;
  try { detail::AudioFrameOutput duplicate(stream); } catch (const std::invalid_argument&) { refused = true; }
  require(refused);
  auto floating = std::make_shared<AudioFrameStream>(AudioStreamOptions{8, 16, AudioSampleFormat::float32});
  detail::AudioFrameOutput float_output(floating); float_output.advance();
  const auto channel = float_output.add("float");
  const int16_t samples[] = {-32768, -16384, 0, 32767};
  float_output.submit("float", channel, samples, 16, 8000, 2, 2, {});
  auto fp = floating->read(0ms).frame;
  require(fp && fp->samples.empty() && fp->float_samples == std::vector<float>({-1.0f, -0.5f, 0.0f, 32767.0f / 32768}));
  require(fp->duration() == 250us);
  float_output.submit("float", channel, samples, 16, 8000, 2, 2, {});
  float_output.submit("float", channel, samples, 16, 8000, 2, 2, {});
  require(floating->stats().queued_frames == 1 && floating->stats().dropped == 1);
  refused = false;
  try { float_output.submit("float", channel, samples, 32, 8000, 2, 2, {}); } catch (const SDKError&) { refused = true; }
  require(refused);
}
