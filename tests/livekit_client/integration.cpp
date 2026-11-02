#include "livekit/client/room.hpp"
#include "rtc_video_fixtures.hpp"
#include "modules/audio_coding/codecs/opus/opus_interface.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>

using namespace livekit::sdk::client;
using namespace std::chrono_literals;

namespace {
void check(bool value, const char* message) {
  if (!value) {
    livekit::sdk::test::fail(message);
  }
}

struct Events : RoomObserver {
  std::atomic<int> data{0};

  void on_room_event(const RoomEvent& event) override {
    if (event.kind == RoomEventKind::data_received && event.data &&
        event.data->payload == std::vector<uint8_t>({1, 2, 3, 4})) {
      ++data;
    }
  }
};

void little(std::ostream& out, uint64_t value, int bytes) {
  for (int i = 0; i < bytes; ++i) {
    out.put(char(value >> (8 * i)));
  }
}
}  // namespace

int main() {
  const char* url = std::getenv("LIVEKIT_URL");
  const char* publisher_token = std::getenv("LIVEKIT_PUBLISH_TOKEN");
  const char* subscriber_token = std::getenv("LIVEKIT_SUBSCRIBE_TOKEN");
  if (!url || !publisher_token || !subscriber_token) {
    std::cout << "SKIP: set LIVEKIT_URL, LIVEKIT_PUBLISH_TOKEN, LIVEKIT_SUBSCRIBE_TOKEN\n";
    return 77;
  }
  Events events;
  Room publisher, subscriber;
  subscriber.set_observer(&events);
  ConnectionOptions options;
  options.url = url;
  options.token = publisher_token;
  options.connect_timeout = 15;
  options.operation_timeout = 10s;
  {
    Room rejected;
    auto invalid = options;
    invalid.token = "invalid-test-token";
    invalid.connect_timeout = 3;
    check(!rejected.connect(invalid), "invalid credentials fail connect");
    rejected.close();
  }
  check(publisher.connect(options), "publisher connect");
  options.token = subscriber_token;
  check(subscriber.connect(options), "subscriber connect");
  CodecFormat opus;
  opus.codec = Codec::opus;
  opus.clock_rate = 48000;
  opus.channels = 1;
  auto audio = LocalTrack::create_audio("encoded-audio", opus);
  CodecFormat vp8;
  vp8.codec = Codec::vp8;
  vp8.width = 64;
  vp8.height = 48;
  auto video = LocalTrack::create_video("encoded-video", vp8);
  check(bool(audio) && bool(video), "track creation");
  check(bool(publisher.local_participant().publish_track(*audio)), "publish audio");
  check(bool(publisher.local_participant().publish_track(*video)), "publish video");
  std::optional<RemoteTrack> rx_audio, rx_video;
  const auto deadline = SteadyClock::now() + 15s;
  while (!rx_audio || !rx_video) {
    for (auto participant : subscriber.remote_participants()) {
      for (auto publication : participant.track_publications()) {
        if (publication.snapshot().kind == MediaKind::audio) {
          rx_audio = publication.track();
        } else {
          rx_video = publication.track();
        }
      }
    }
    check(SteadyClock::now() < deadline, "automatic subscriptions ready");
    std::this_thread::sleep_for(10ms);
  }
  publisher.local_participant().publish_data({{1, 2, 3, 4}, true, "client-test", {}});
  OpusDecInst* decoder = nullptr;
  check(WebRtcOpus_DecoderCreate(&decoder, 1, 48000) == 0, "test Opus decoder");
  std::ofstream ivf("livekit-client-received.ivf", std::ios::binary);
  check(bool(ivf), "open received IVF");
  ivf.write("DKIF", 4);
  little(ivf, 0, 2);
  little(ivf, 32, 2);
  ivf.write("VP80", 4);
  little(ivf, 64, 2);
  little(ivf, 48, 2);
  little(ivf, 90000, 4);
  little(ivf, 1, 4);
  little(ivf, 0, 4);
  little(ivf, 0, 4);
  int audio_count = 0, video_count = 0;
  uint64_t last_audio = 0, last_video = 0;
  for (uint64_t i = 0; i < 150; ++i) {
    EncodedFrame af;
    af.buffer = EncodedBuffer({0xf8, 0xff, 0xfe});  // Valid Opus silence access unit.
    af.epoch = audio->feedback().epoch;
    af.timestamp = i * 960;
    af.duration = 20ms;
    check(audio->try_write(std::move(af)) == WriteStatus::accepted, "audio write");
    if (i % 2 == 0) {
      EncodedFrame vf;
      vf.buffer = EncodedBuffer(fixture_video(livekit::sdk::rtc::Codec::vp8, (i / 2) % 2 != 0));
      vf.epoch = video->feedback().epoch;
      vf.timestamp = i * 1800;
      vf.duration = 40ms;
      vf.keyframe = true;
      check(video->try_write(std::move(vf)) == WriteStatus::accepted, "video write");
    }
    auto a = rx_audio->read({20ms, {}});
    if (a.status == ReadStatus::frame) {
      int16_t pcm[5760] = {}, type = 0;
      auto bytes = a.frame->buffer.bytes();
      check(WebRtcOpus_Decode(decoder, bytes.data(), bytes.size(), pcm, &type) > 0,
            "received Opus decodes");
      check(!audio_count || a.frame->timestamp >= last_audio, "audio timestamps monotonic");
      last_audio = a.frame->timestamp;
      ++audio_count;
    }
    auto v = rx_video->read({1ms, {}});
    if (v.status == ReadStatus::frame) {
      check(!video_count || v.frame->timestamp >= last_video, "video timestamps monotonic");
      last_video = v.frame->timestamp;
      auto bytes = v.frame->buffer.bytes();
      little(ivf, bytes.size(), 4);
      little(ivf, v.frame->timestamp, 8);
      ivf.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      ++video_count;
    }
    std::this_thread::sleep_for(10ms);
  }
  ivf.seekp(24);
  little(ivf, video_count, 4);
  ivf.close();
  check(audio_count > 20 && video_count > 10, "sustained encoded media delivery");
  check(events.data > 0, "data message observed");
  if (const char* marker = std::getenv("LIVEKIT_CLIENT_RESTART_MARKER")) {
    const auto old_epoch = audio->feedback().epoch;
    const auto old_sid = audio->snapshot().server_sid;
    std::ofstream(marker) << "ready\n";
    const auto recovery_deadline = SteadyClock::now() + 45s;
    int recovered_audio = 0, recovered_video = 0;
    uint64_t index = 300;
    while (recovered_audio < 20 || recovered_video < 10) {
      check(SteadyClock::now() < recovery_deadline, "server restart media recovery deadline");
      ++index;
      const auto epoch = audio->feedback().epoch;
      if (epoch > old_epoch && audio->snapshot().state == TrackState::ready &&
          video->snapshot().state == TrackState::ready &&
          subscriber.connection_state() == ConnectionState::connected) {
        rx_audio.reset();
        rx_video.reset();
        for (auto participant : subscriber.remote_participants()) {
          for (auto publication : participant.track_publications()) {
            if (publication.snapshot().kind == MediaKind::audio) {
              rx_audio = publication.track();
            } else {
              rx_video = publication.track();
            }
          }
        }
        EncodedFrame af;
        af.buffer = EncodedBuffer({0xf8, 0xff, 0xfe});
        af.epoch = epoch;
        af.timestamp = index * 960;
        af.duration = 20ms;
        (void)audio->try_write(std::move(af));
        EncodedFrame vf;
        vf.buffer = EncodedBuffer(fixture_video(livekit::sdk::rtc::Codec::vp8, index % 2 != 0));
        vf.epoch = video->feedback().epoch;
        vf.timestamp = index * 1800;
        vf.duration = 20ms;
        vf.keyframe = true;
        (void)video->try_write(std::move(vf));
        if (rx_audio) {
          auto received = rx_audio->read({10ms, {}});
          if (received.status == ReadStatus::frame) {
            int16_t pcm[5760] = {}, type = 0;
            auto bytes = received.frame->buffer.bytes();
            check(WebRtcOpus_Decode(decoder, bytes.data(), bytes.size(), pcm, &type) > 0,
                  "recovered Opus decodes");
            ++recovered_audio;
          }
        }
        if (rx_video) {
          auto received = rx_video->read({1ms, {}});
          if (received.status == ReadStatus::frame) {
            ++recovered_video;
          }
        }
      }
      std::this_thread::sleep_for(20ms);
    }
    check(audio->snapshot().server_sid != old_sid, "full rejoin updates publication SID");
    std::cout << "PASS restart audio_decoded=" << recovered_audio
              << " video_frames=" << recovered_video << " epoch=" << audio->feedback().epoch
              << '\n';
  }
  WebRtcOpus_DecoderFree(decoder);
  video->set_muted(true);
  check(video->snapshot().muted, "synchronous local mute");
  video->set_muted(false);
  check(!video->snapshot().muted, "synchronous local unmute");
  publisher.local_participant().unpublish_track(video->snapshot().server_sid);
  check(publisher.local_participant().track_publications().size() == 1, "native unpublish video");
  publisher.local_participant().unpublish_track(audio->snapshot().server_sid);
  check(publisher.local_participant().track_publications().empty(), "native unpublish audio");
  subscriber.close();
  publisher.close();
  std::cout << "PASS audio_decoded=" << audio_count << " video_frames=" << video_count
            << " data=" << events.data << '\n';
}
