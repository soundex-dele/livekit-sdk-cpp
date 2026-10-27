#pragma once
#include <livekit/livekit.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <numbers>
#include <set>
#include <thread>

namespace audio_source_test {
using namespace livekit::sdk;
using namespace std::chrono_literals;
inline void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void until(F predicate, const char* message) {
  const auto end = std::chrono::steady_clock::now() + 15s;
  while (!predicate()) { check(std::chrono::steady_clock::now() < end, message); std::this_thread::sleep_for(10ms); }
}
struct Tone {
  int previous = 0, rate = 0, peak = 0;
  uint64_t count = 0, crossings = 0;
  void add(const AudioFrame& frame, size_t channel) {
    check(frame.channels > channel && frame.format == AudioSampleFormat::pcm16, "Wrong decoded channel layout");
    rate = frame.sample_rate;
    for (size_t i = 0; i < frame.samples_per_channel; ++i) {
      const auto sample = frame.samples[i * frame.channels + channel];
      if (previous < 0 && sample >= 0) ++crossings;
      previous = sample; ++count; peak = std::max(peak, std::abs(int(sample)));
    }
  }
  double frequency() const { return count ? double(crossings) * rate / count : 0; }
};
inline int run(const char* server, const char* proxy, const char* directory) {
  auto token = [](const char* identity) {
    return make_token("reconnect-test", "reconnect-integration-secret-32-characters", identity,
      {{"roomJoin", true}, {"room", "audio-source"}, {"canPublish", true}, {"canSubscribe", true},
       {"canPublishData", true}, {"canUpdateOwnMetadata", true}});
  };
  auto mono = std::make_shared<PcmAudioSource>(PcmSourceOptions{16000,1,AudioSampleFormat::pcm16});
  auto stereo = std::make_shared<PcmAudioSource>(PcmSourceOptions{44100,2,AudioSampleFormat::float32});
  auto stream = std::make_shared<AudioFrameStream>(AudioStreamOptions{1000,4 * 1024 * 1024});
  std::atomic<int> rejoins{0}, rejected{0}; std::atomic<bool> pause_mono{false};
  RoomCallback callbacks; callbacks.on_reconnected = [&](const ReconnectInfo& info) { if (info.mode == ReconnectMode::rejoin) ++rejoins; };
  Room publisher(callbacks), reader;
  RoomOptions po; po.signal_protocol = 15;
  publisher.join_with_token(proxy, token("pcm-writer"), po);
  RoomOptions ro; ro.audio_frame_stream = stream; reader.join_with_token(server, token("pcm-reader"), ro);
  MediaSpec media; media.video = false; media.audio_source = mono;
  auto first = publisher.local_participant().publish_track(media, {"mono-440"}).get();
  media.audio_source = stereo;
  auto second = publisher.local_participant().publish_track(media, {"stereo-660-880"}).get();
  const auto first_id = first.info().client_id, second_id = second.info().client_id;
  auto feed = [&](std::stop_token stop, std::shared_ptr<PcmAudioSource> source) {
    const auto f = source->options(); uint64_t index = 0;
    while (!stop.stop_requested()) {
      if (source == mono && pause_mono) { std::this_thread::sleep_for(2ms); continue; }
      PcmInputFrame frame; const auto n = f.sample_rate / 50;
      if (f.format == AudioSampleFormat::pcm16) frame.samples.resize(n * f.channels);
      else frame.float_samples.resize(n * f.channels);
      for (int i = 0; i < n; ++i) for (size_t channel = 0; channel < f.channels; ++channel) {
        const double frequency = source == mono ? 440 : channel == 0 ? 660 : 880;
        const auto value = .2 * std::sin(2 * std::numbers::pi * frequency * (index + i) / f.sample_rate);
        if (f.format == AudioSampleFormat::pcm16) frame.samples[i * f.channels + channel] = static_cast<int16_t>(value * 32768);
        else frame.float_samples[i * f.channels + channel] = static_cast<float>(value);
      }
      const auto status = source->write(frame);
      if (status == AudioWriteStatus::accepted) index += n;
      else if (status == AudioWriteStatus::backpressure) ++rejected;
      else break;
      std::this_thread::sleep_for(2ms);
    }
  };
  std::jthread mono_writer([&](std::stop_token stop) { feed(stop, mono); });
  std::jthread stereo_writer([&](std::stop_token stop) { feed(stop, stereo); });
  auto measure = [&](const std::string& mono_sid, const std::string& stereo_sid) {
    std::set<std::string> audible;
    const auto end = std::chrono::steady_clock::now() + 12s;
    while (audible.size() != 2) {
      check(std::chrono::steady_clock::now() < end, "PCM inputs never became audible");
      const auto result = stream->read(1s); check(result.status == AudioReadStatus::frame, "PCM input output stalled");
      const auto& f = *result.frame;
      if ((f.track_id == mono_sid || f.track_id == stereo_sid) &&
          std::any_of(f.samples.begin(), f.samples.end(), [](auto sample) { return std::abs(int(sample)) > 500; })) audible.insert(f.track_id);
    }
    Tone low, left, right;
    while (low.count < 48000 || left.count < 48000) {
      check(std::chrono::steady_clock::now() < end, "PCM input frequency measurement timed out");
      const auto value = stream->read(1s); check(value.status == AudioReadStatus::frame, "PCM input reader stopped");
      if (value.frame->track_id == mono_sid) low.add(*value.frame, 0);
      else if (value.frame->track_id == stereo_sid) {
        if (value.frame->channels != 2) std::cerr << "Stereo decoder channels=" << value.frame->channels << '\n';
        check(value.frame->channels == 2, "Stereo publication was downmixed");
        left.add(*value.frame, 0); right.add(*value.frame, 1);
      }
    }
    Json values{{"mono", low.frequency()}, {"left", left.frequency()}, {"right", right.frequency()}};
    std::cerr << values.dump() << '\n';
    check(std::abs(low.frequency() - 440) < 22 && std::abs(left.frequency() - 660) < 33 &&
        std::abs(right.frequency() - 880) < 44 && low.peak > 500 && left.peak > 500 && right.peak > 500,
        "Resampled PCM had wrong frequency, channel isolation or amplitude");
    return values;
  };
  auto initial = measure(first.sid(), second.sid());
  bool refused = false;
  try { reader.local_participant().publish_track(media).get(); } catch (const std::exception&) { refused = true; }
  check(refused && stereo->stats().accepting, "Another Room reused PCM source");
  check(mono->stats().queued_blocks <= 20 && stereo->stats().queued_blocks <= 20 && rejected > 0, "PCM input queue was not bounded");
  pause_mono = true; const auto silence = mono->stats().silence_samples;
  until([&] { return mono->stats().silence_samples >= silence + 1600; }, "Audio underflow did not produce accounted silence");
  publisher.local_participant().set_metadata("audio-underflow", OperationOptions{2s, {}, true}).get();
  const auto generation = mono->stats().generation;
  std::ofstream(std::filesystem::path(directory) / "pcm-source-rejoin") << "ready";
  until([&] { return rejoins == 1 && mono->stats().generation > generation && stereo->stats().generation > generation; }, "PCM source recovery failed");
  pause_mono = false;
  auto tracks = publisher.local_participant().track_publications();
  check(tracks.size() == 2, "PCM rejoin duplicated a track");
  std::string mono_sid, stereo_sid;
  for (const auto& track : tracks) {
    if (track.info().client_id == first_id) mono_sid = track.sid();
    if (track.info().client_id == second_id) stereo_sid = track.sid();
  }
  check(!mono_sid.empty() && !stereo_sid.empty(), "PCM stable IDs changed on rejoin");
  auto recovered = measure(mono_sid, stereo_sid);
  refused = false; try { first.set_muted(false).get(); } catch (const std::exception&) { refused = true; }
  check(refused, "Old PCM publication survived rejoin");
  publisher.local_participant().unpublish_track(mono_sid).get();
  until([&] { return !mono->stats().attached; }, "Unpublish did not release PCM source");
  check(mono->stats().queued_blocks == 0 && !mono->stats().accepting && stereo->stats().accepting, "Stopping one PCM track stopped another");
  mono->close(); stereo->close(); mono_writer.request_stop(); stereo_writer.request_stop(); mono_writer.join(); stereo_writer.join();
  PcmInputFrame empty; check(mono->write(empty) == AudioWriteStatus::closed, "PCM EOS not permanent");
  publisher.local_participant().unpublish_track(stereo_sid).get();
  media.audio_source = std::make_shared<PcmAudioSource>();
  publisher.local_participant().publish_track(media).get();
  until([&] { return media.audio_source->stats().accepting; }, "Empty PCM source not activated");
  const auto before = std::chrono::steady_clock::now();
  publisher.disconnect(); publisher.wait(); reader.disconnect(); reader.wait();
  check(std::chrono::steady_clock::now() - before < 3s && !media.audio_source->stats().attached, "Empty PCM source shutdown blocked");
  std::cout << Json{{"ok", true}, {"initialFrequencies", initial}, {"recoveredFrequencies", recovered}, {"rejoins", rejoins.load()},
    {"backpressure", rejected.load()}, {"silenceSamples", mono->stats().silence_samples}, {"droppedSamples", mono->stats().dropped_samples + stereo->stats().dropped_samples},
    {"schedulerSkippedSamples", mono->stats().skipped_samples + stereo->stats().skipped_samples}, {"independentTracks", true}, {"stereo", true},
    {"stableIds", true}, {"emptySourceShutdown", true}, {"received", stream->stats().received}}.dump() << '\n';
  return 0;
}
}
