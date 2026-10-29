#include "media_nodes/playout/audio_output_driver.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <mutex>

namespace livekit::sdk::rtc {
using namespace std::chrono_literals;
struct AudioOutputTrack::State {
  struct Packet { uint64_t at; std::vector<int16_t> stereo; uint64_t end() const { return at+stereo.size()/2; } };
  TrackId id;
  AudioOutputTrackSnapshot stats;
  std::deque<Packet> queue;
  std::optional<uint64_t> base_timestamp,last_end;
  uint64_t base_play = 0;
  float gain = 1;
  SteadyClock::time_point boundary{};
  void reset() {
    stats.dropped+=queue.size(); queue.clear(); stats.queued_samples=0;
    base_timestamp.reset(); last_end.reset(); ++stats.resets;
  }
};
struct AudioOutputSession::Impl {
  const AudioOutputOptions options;
  std::unique_ptr<detail::AudioOutputDriver> driver;
  std::mutex control;
  mutable std::mutex mutex;
  std::map<TrackId,std::shared_ptr<AudioOutputTrack::State>> tracks;
  AudioOutputSnapshot stats;
  std::atomic<uint64_t> cursor{0},contention{0};
  Impl(AudioOutputOptions options,std::unique_ptr<detail::AudioOutputDriver> driver)
    : options(options),driver(std::move(driver)) {
    if (!this->driver || !options.max_tracks || options.max_tracks>128 || options.buffer<0ms ||
        options.buffer>200ms || options.max_latency<120ms || options.max_latency>2s || options.buffer>=options.max_latency)
      throw std::invalid_argument("Invalid audio output device options");
  }
  void render(std::span<int16_t> output) noexcept {
    std::fill(output.begin(),output.end(),0);
    if (output.size()!=960) return;
    const auto begin=cursor.fetch_add(480),end=begin+480;
    std::unique_lock lock(mutex,std::try_to_lock);
    if (!lock) { ++contention; return; }
    if (!stats.error.empty()) return;
    std::array<int32_t,960> mix{};
    for (const auto& [id,state]:tracks) {
      auto& s=*state; size_t rendered=0;
      for (const auto& packet:s.queue) {
        if (packet.at>=end) break;
        const auto first=std::max(begin,packet.at),last=std::min(end,packet.end());
        if (last<=first) continue;
        for (auto at=first;at<last;++at) for(size_t c=0;c<2;++c)
          mix[(at-begin)*2+c]+=int32_t(std::lround(packet.stereo[(at-packet.at)*2+c]*s.gain));
        rendered+=size_t(last-first);
      }
      while (!s.queue.empty() && s.queue.front().end()<=end) {
        if(s.queue.front().end()<=begin)++s.stats.dropped;
        s.stats.queued_samples-=s.queue.front().stereo.size()/2; s.queue.pop_front();
      }
      s.stats.rendered_samples+=rendered; s.stats.silence_samples+=480-rendered;
    }
    for(size_t i=0;i<output.size();++i)output[i]=int16_t(std::clamp(mix[i],-32768,32767));
    ++stats.rendered_blocks;
  }
};
AudioOutputSession::AudioOutputSession(AudioOutputOptions options)
  : AudioOutputSession(options,detail::create_native_audio_output_driver()) {}
AudioOutputSession::AudioOutputSession(AudioOutputOptions options,std::unique_ptr<detail::AudioOutputDriver> driver)
  : impl_(std::make_unique<Impl>(options,std::move(driver))) {}
AudioOutputSession::~AudioOutputSession() { impl_->driver->stop(); }
std::shared_ptr<AudioOutputTrack> AudioOutputSession::open_track(TrackId id) {
  auto self=shared_from_this(); auto& s=*impl_;
  auto state=std::make_shared<AudioOutputTrack::State>(); state->id=std::move(id);
  state->stats.closed=true;
  auto track=std::shared_ptr<AudioOutputTrack>(new AudioOutputTrack(std::move(self),state));
  std::lock_guard control(s.control);
  bool first;
  {
    std::lock_guard lock(s.mutex);
    if (!s.stats.error.empty()) throw std::runtime_error(s.stats.error);
    if (state->id.empty() || s.tracks.contains(state->id) || s.tracks.size()>=s.options.max_tracks)
      throw std::invalid_argument("Duplicate, empty or excess audio output track");
    first=s.tracks.empty(); s.tracks.emplace(state->id,state); state->stats.closed=false; s.stats.tracks=s.tracks.size();
  }
  if (first) {
    try {
      s.driver->start(s.options,[&s](auto output) { s.render(output); },[&s](std::string message) {
        std::lock_guard lock(s.mutex);
        if(s.stats.error.empty()) { s.stats.error=std::move(message); for(const auto& [id,track]:s.tracks)track->reset(); }
      });
      std::lock_guard lock(s.mutex); s.stats.running=true;
    } catch (...) {
      s.driver->stop(); std::lock_guard lock(s.mutex);
      s.tracks.erase(state->id); s.stats.tracks=s.tracks.size(); state->stats.closed=true;
      throw;
    }
  }
  return track;
}
AudioOutputSnapshot AudioOutputSession::snapshot() const {
  std::lock_guard lock(impl_->mutex); auto result=impl_->stats; result.contention_blocks=impl_->contention; return result;
}
void AudioOutputSession::throw_if_failed() const { const auto state=snapshot(); if(!state.error.empty())throw std::runtime_error(state.error); }
AudioOutputTrack::AudioOutputTrack(std::shared_ptr<AudioOutputSession> session,std::shared_ptr<State> state)
  : session_(std::move(session)),state_(std::move(state)) {}
AudioOutputTrack::~AudioOutputTrack() { close(); }
bool AudioOutputTrack::write(const nodes::AudioFrameEvent& event) {
  auto& device=*session_->impl_; auto& s=*state_;
  const auto invalid=[&] { std::lock_guard lock(device.mutex); ++s.stats.dropped; return false; };
  if (!event.pcm || !event.epoch || event.timestamp>std::numeric_limits<uint64_t>::max()-5760) return invalid();
  const auto& pcm=*event.pcm;
  const auto frames=pcm.samples_per_channel();
  if (pcm.sample_rate!=48000 || (pcm.channels!=1 && pcm.channels!=2) || pcm.format!=PcmFormat::s16 ||
      !pcm.float_samples.empty() || pcm.samples.size()!=frames*pcm.channels || frames<120 || frames>5760 || frames%120) return invalid();
  State::Packet packet; packet.stereo.resize(frames*2);
  for(size_t i=0;i<frames;++i) for(size_t c=0;c<2;++c)packet.stereo[i*2+c]=pcm.samples[i*pcm.channels+(pcm.channels==1?0:c)];
  std::lock_guard lock(device.mutex);
  const auto now=SteadyClock::now();
  if (s.stats.closed || !device.stats.error.empty() || event.epoch<s.stats.epoch ||
      event.captured<s.boundary || event.captured>now+1s || now-event.captured>device.options.max_latency) { ++s.stats.dropped; return false; }
  if (event.epoch!=s.stats.epoch) { s.reset(); s.stats.epoch=event.epoch; }
  if (s.stats.muted) { ++s.stats.dropped; return false; }
  if (s.last_end && event.timestamp<*s.last_end) { ++s.stats.dropped; return false; }
  const auto cursor=device.cursor.load();
  const uint64_t maximum=uint64_t(device.options.max_latency.count())*48;
  const auto anchor=[&] { s.base_timestamp=event.timestamp; s.base_play=cursor+std::min(uint64_t(device.options.buffer.count())*48,maximum-frames); };
  if(!s.base_timestamp)anchor();
  auto delta=event.timestamp-*s.base_timestamp;
  // Check relative distance before addition to reject/wrap huge RTP jumps safely.
  const auto elapsed=cursor>=s.base_play ? cursor-s.base_play : 0;
  if ((delta>elapsed && delta-elapsed>maximum) ||
      (elapsed>delta+frames && elapsed-delta-frames>maximum) || s.stats.queued_samples+frames>maximum) {
    s.reset(); anchor(); delta=0;
  }
  packet.at=s.base_play+delta;
  if(packet.end()>cursor+maximum) { s.reset(); anchor(); packet.at=s.base_play; }
  if (packet.end()<=cursor) { ++s.stats.dropped; return false; }
  s.last_end=event.timestamp+frames; s.stats.queued_samples+=frames; ++s.stats.admitted;
  s.queue.push_back(std::move(packet)); return true;
}
void AudioOutputTrack::set_gain(float gain) {
  if (!std::isfinite(gain) || gain<0 || gain>4) throw std::invalid_argument("Audio output gain must be 0..4");
  std::lock_guard lock(session_->impl_->mutex); state_->gain=gain;
}
void AudioOutputTrack::set_muted(bool muted) {
  std::lock_guard lock(session_->impl_->mutex);
  if(state_->stats.muted!=muted) { state_->stats.muted=muted; state_->reset(); state_->boundary=SteadyClock::now(); }
}
void AudioOutputTrack::close() {
  auto& device=*session_->impl_; std::lock_guard control(device.control);
  bool last=false;
  {
    std::lock_guard lock(device.mutex);
    if(state_->stats.closed)return;
    state_->stats.closed=true; state_->reset();
    const auto found=device.tracks.find(state_->id);
    if(found!=device.tracks.end() && found->second==state_)device.tracks.erase(found);
    device.stats.tracks=device.tracks.size(); last=device.tracks.empty();
  }
  if(last) { device.driver->stop(); std::lock_guard lock(device.mutex); device.stats.running=false; }
}
AudioOutputTrackSnapshot AudioOutputTrack::snapshot() const { std::lock_guard lock(session_->impl_->mutex); return state_->stats; }
namespace nodes {
struct AudioOutputNode::Impl {
  TrackId id;
  std::shared_ptr<AudioOutputSession> session;
  mutable std::mutex mutex;
  std::shared_ptr<AudioOutputTrack> track;
  bool closed=false;
  float gain=1;
  bool muted=false;
};
AudioOutputNode::AudioOutputNode(std::string name,TrackId id,std::shared_ptr<AudioOutputSession> session)
  : GraphNode(name,nullptr),impl_(std::make_unique<Impl>()) {
  if(id.empty() || !session)throw std::invalid_argument("Audio output needs a track ID and device session");
  impl_->id=std::move(id); impl_->session=std::move(session);
}
AudioOutputNode::~AudioOutputNode() { quiesce(); release(); }
bool AudioOutputNode::doInit() {
  subscribe<AudioFrameEvent>([this](const auto& frame) { std::lock_guard lock(impl_->mutex); if(!impl_->closed && impl_->track)impl_->track->write(frame); });
  return true;
}
bool AudioOutputNode::doStart() {
  std::lock_guard lock(impl_->mutex); if(impl_->closed)return false;
  impl_->track=impl_->session->open_track(impl_->id);
  impl_->track->set_gain(impl_->gain); impl_->track->set_muted(impl_->muted); return true;
}
void AudioOutputNode::set_gain(float gain) {
  if(!std::isfinite(gain) || gain<0 || gain>4)throw std::invalid_argument("Audio output gain must be 0..4");
  std::lock_guard lock(impl_->mutex); impl_->gain=gain; if(impl_->track)impl_->track->set_gain(gain);
}
void AudioOutputNode::set_muted(bool muted) {
  std::lock_guard lock(impl_->mutex); impl_->muted=muted; if(impl_->track)impl_->track->set_muted(muted);
}
void AudioOutputNode::quiesce() { std::lock_guard lock(impl_->mutex); impl_->closed=true; if(impl_->track)impl_->track->close(); }
void AudioOutputNode::doStop() { quiesce(); }
AudioOutputTrackSnapshot AudioOutputNode::snapshot() const {
  std::lock_guard lock(impl_->mutex); if(impl_->track)return impl_->track->snapshot();
  AudioOutputTrackSnapshot state; state.closed=impl_->closed; return state;
}
void AudioOutputNode::throw_if_failed() const { impl_->session->throw_if_failed(); }
}
}
