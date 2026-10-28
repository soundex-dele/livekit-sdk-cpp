#pragma once
#include "api/frame_transformer_interface.h"
#include "api/make_ref_counted.h"
#include "audio/channel_send_frame_transformer_delegate.h"
#include "media_nodes/capture/encoded_source.hpp"
#include "media_graph/video_events.hpp"
#include "system_wrappers/include/clock.h"
#include <map>

namespace livekit::sdk::detail {
struct EncodedAccessUnit:graphflow::core::EventT<EncodedAccessUnit>{
  explicit EncodedAccessUnit(EncodedMediaFrame value):frame(std::move(value)){}EncodedMediaFrame frame;
};
class EncodedFrameTransformer:public webrtc::FrameTransformerInterface {
 public:
  EncodedFrameTransformer(std::shared_ptr<RtcGraph> graph,std::shared_ptr<EncodedOutput> output,std::string track,bool video,
                         std::shared_ptr<EncodedSourceReader> input={}):output_(std::move(output)),input_(std::move(input)),track_(std::move(track)),video_(video),pipeline_(std::move(graph)){
    generation_=output_?output_->generation():0;
    ingress_=std::make_shared<MediaEventNode<EncodedAccessUnit>>("EncodedFrameInputNode",NodeExecution::source);
    auto sink=std::make_shared<MediaEventNode<EncodedAccessUnit>>("EncodedFrameOutputNode",NodeExecution::sink,[this](EncodedAccessUnit& event){if(output_)output_->push(std::move(event.frame),generation_);});
    pipeline_.build({ingress_,sink},{RtcPipeline::connect<EncodedAccessUnit>(ingress_,sink)});
    if(!pipeline_.start())throw std::runtime_error("Encoded output graph failed to start");
  }
  ~EncodedFrameTransformer(){pipeline_.release();}
  void RegisterTransformedFrameCallback(webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback)override{std::lock_guard lock(mutex_);callback_=std::move(callback);}
  void UnregisterTransformedFrameCallback()override{std::lock_guard lock(mutex_);callback_=nullptr;}
  void RegisterTransformedFrameSinkCallback(webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback,uint32_t ssrc)override{std::lock_guard lock(mutex_);sinks_[ssrc]=std::move(callback);}
  void UnregisterTransformedFrameSinkCallback(uint32_t ssrc)override{std::lock_guard lock(mutex_);sinks_.erase(ssrc);}
  void Transform(std::unique_ptr<webrtc::TransformableFrameInterface> frame)override{
    webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback;
    {std::lock_guard lock(mutex_);auto it=sinks_.find(frame->GetSsrc());callback=it==sinks_.end()?callback_:it->second;}
    if(!callback)return;
    try{
      if(input_){
        auto* audio=dynamic_cast<webrtc::TransformableAudioFrameInterface*>(frame.get());if(!audio)return;
        // Native Opus cadence supplies transport metadata only. Encoded source
        // samples are never decoded or re-encoded; preserve each access unit.
        for(int n=0;n<8;++n){auto source=input_->read({},false);if(!source)break;
          auto clone=webrtc::CloneSenderAudioFrame(audio);clone->SetData({source->data.data(),source->data.size()});
          const auto us=std::chrono::duration_cast<std::chrono::microseconds>(source->captured.time_since_epoch()).count();
          clone->SetRTPTimestamp(uint32_t(us*48/1000));callback->OnTransformedFrame(std::move(clone));
        }return;
      }
      if(output_){
        EncodedMediaFrame copy;copy.track_id=track_;copy.codec=frame->GetMimeType();copy.rtp_timestamp=frame->GetTimestamp();
        copy.data.assign(frame->GetData().begin(),frame->GetData().end());copy.captured=std::chrono::steady_clock::now();
        if(video_)if(auto* video=dynamic_cast<webrtc::TransformableVideoFrameInterface*>(frame.get())){copy.keyframe=video->IsKeyFrame();auto metadata=video->Metadata();copy.width=metadata.GetWidth();copy.height=metadata.GetHeight();}
        ingress_->emit(EncodedAccessUnit(std::move(copy)));
      }
      callback->OnTransformedFrame(std::move(frame));
    }catch(...){/* An observer failure must not interrupt native decoding. */ if(!input_&&frame)callback->OnTransformedFrame(std::move(frame));}
  }
 private:
  std::mutex mutex_;webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback_;std::map<uint32_t,webrtc::scoped_refptr<webrtc::TransformedFrameCallback>> sinks_;
  std::shared_ptr<EncodedOutput> output_;std::shared_ptr<EncodedSourceReader> input_;std::string track_;bool video_;uint64_t generation_;
  std::shared_ptr<MediaEventNode<EncodedAccessUnit>> ingress_;RtcPipeline pipeline_;
};
}
