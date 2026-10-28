#pragma once
#include "engines/rtc/rtc_graph.hpp"
#include "modules/audio_mixer/audio_mixer_impl.h"
#include "system_wrappers/include/clock.h"
#include <map>

namespace livekit::sdk::detail {
// Observe the actual per-source samples pulled for device mixing, including
// NetEq's RTP/SR-derived capture time. AudioTrack sinks omit this timestamp.
class GraphAudioMixer:public webrtc::AudioMixer {
  struct SourceProxy:Source {
    SourceProxy(Source* source,std::shared_ptr<RtcGraph> graph):source(source),graph(std::move(graph)){}
    AudioFrameInfo GetAudioFrameWithInfo(int rate,webrtc::AudioFrame* frame)override{
      auto status=source->GetAudioFrameWithInfo(rate,frame);
      if(status!=AudioFrameInfo::kError&&frame->ntp_time_ms_>0)
        graph->audio_playout(uint32_t(source->Ssrc()),frame->ntp_time_ms_,webrtc::Clock::GetRealTimeClock()->TimeInMicroseconds());
      return status;
    }
    int Ssrc()const override{return source->Ssrc();}
    int PreferredSampleRate()const override{return source->PreferredSampleRate();}
    Source* source;std::shared_ptr<RtcGraph> graph;
  };
 public:
  explicit GraphAudioMixer(std::shared_ptr<RtcGraph> graph):graph_(std::move(graph)),backend_(webrtc::AudioMixerImpl::Create()){}
  bool AddSource(Source* source)override{
    std::lock_guard lock(mutex_);if(sources_.contains(source))return false;
    auto proxy=std::make_unique<SourceProxy>(source,graph_);if(!backend_->AddSource(proxy.get()))return false;sources_[source]=std::move(proxy);return true;
  }
  void RemoveSource(Source* source)override{std::lock_guard lock(mutex_);auto it=sources_.find(source);if(it!=sources_.end()){backend_->RemoveSource(it->second.get());sources_.erase(it);}}
  void Mix(size_t channels,webrtc::AudioFrame* frame)override{backend_->Mix(channels,frame);}
 private:
  std::shared_ptr<RtcGraph> graph_;webrtc::scoped_refptr<webrtc::AudioMixer> backend_;std::mutex mutex_;std::map<Source*,std::unique_ptr<SourceProxy>> sources_;
};
}
