#include "livekit/rtc/video_nodes.hpp"
#include "livekit/rtc/stream_pipeline.hpp"
#include "rtc_video_fixtures.hpp"
#include <condition_variable>
#include <iostream>
#include <limits>
#include <thread>
using namespace livekit::sdk::rtc;
using namespace std::chrono_literals;
namespace {
void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
template<class F> void eventually(F f,const char* message) {
  const auto deadline=SteadyClock::now()+5s;
  while(!f()) { check(SteadyClock::now()<deadline,message); std::this_thread::sleep_for(1ms); }
}
TrackDescriptor video() { TrackDescriptor d; d.id="video"; d.format.codec=Codec::vp8; d.format.width=64; d.format.height=48; return d; }
class Input : public graphflow::core::GraphNode {
 public:
  Input() : GraphNode("input",nullptr) {}
  std::atomic<int> plis{0};
  void feedback(Epoch epoch, uint64_t revision=1, bool active=true, uint64_t generation=0) {
    SenderFeedback f; f.epoch=epoch; f.revision=revision; f.encodings[""]={400000,active,true,1}; f.media_generation=generation;
    publish(nodes::SenderFeedbackEvent(f));
  }
  void raw(Epoch epoch,uint64_t timestamp,bool invalid=false,int width=64,std::optional<SteadyClock::time_point> captured={}) {
    auto p=std::make_shared<livekit::sdk::VideoFrame>(); p->width=width; p->height=48;
    p->stride_y=64; p->stride_u=p->stride_v=32;
    p->y.assign(invalid ? 1 : 64*48,80); p->u.assign(32*24,96); p->v.assign(32*24,160);
    nodes::VideoFrameEvent value; value.pixels=p; value.epoch=epoch; value.timestamp=timestamp;
    if(captured)value.captured=*captured; publish(value);
  }
  void encoded(Epoch epoch,uint64_t timestamp,bool key=true) {
    EncodedFrame frame; frame.buffer=EncodedBuffer(fixture_video(Codec::vp8,false)); frame.timestamp=timestamp; frame.epoch=epoch; frame.keyframe=key;
    publish(nodes::EncodedFrameEvent(std::move(frame)));
  }
 protected:
  bool doInit() override { subscribe<nodes::KeyframeRequestEvent>([this](const auto&) { ++plis; }); return true; }
};
class Output : public graphflow::core::GraphNode {
 public:
  Output() : GraphNode("output",nullptr) {}
  std::mutex mutex; std::condition_variable changed;
  bool block=false,entered=false;
  std::vector<EncodedFrame> encoded;
  std::vector<nodes::VideoFrameEvent> raw;
  void unblock() { std::lock_guard lock(mutex); block=false; changed.notify_all(); }
 protected:
  bool doInit() override {
    subscribe<nodes::EncodedFrameEvent>([this](const auto& event) {
      std::unique_lock lock(mutex); encoded.push_back(event.frame); entered=true; changed.notify_all();
      changed.wait(lock,[&] { return !block; });
    });
    subscribe<nodes::VideoFrameEvent>([this](const auto& event) { std::lock_guard lock(mutex); raw.push_back(event); changed.notify_all(); });
    return true;
  }
};
void encoder_queue() {
  auto in=std::make_shared<Input>(); auto out=std::make_shared<Output>(); out->block=true;
  auto codec=std::make_shared<nodes::VideoCodecNode>("encoder",video(),true);
  graphflow::engine::GraphEngine graph{1};
  using G=engine::TrackGraphSpec;
  check(graph.loadFromNodes({out,codec,in},{G::connect<nodes::VideoFrameEvent>(in,codec),G::connect<nodes::SenderFeedbackEvent>(in,codec),G::connect<nodes::EncodedFrameEvent>(codec,out)}) && graph.init() && graph.start(),"Encoder graph start failed");
  // Always release the deliberately blocked callback, including assertion failure.
  struct Unblock { std::shared_ptr<Output> output; ~Unblock() { output->unblock(); } } cleanup{out};
  in->feedback(1); in->raw(1,3001);
  eventually([&] { std::lock_guard lock(out->mutex); return out->entered; },"Encoder callback never arrived");
  for(int i=2;i<40;++i) in->raw(1,uint64_t(i)*3001);
  auto stats=codec->snapshot();
  check(stats.queued_frames<=2 && stats.queued_bytes<=32*1024*1024 && stats.dropped>0,"Slow codec output escaped queue bounds");
  in->feedback(1,2,false,1);
  check(codec->snapshot().queued_frames==0,"Video mute retained queued pixels");
  const auto muted_admissions=codec->snapshot().admitted;
  in->raw(1,150000);
  check(codec->snapshot().admitted==muted_admissions,"Inactive video codec admitted pixels");
  in->feedback(1,3,true,2); in->raw(1,160000);
  check(codec->snapshot().queued_frames==1,"Video unmute did not resume admission");
  const auto held_capture=SteadyClock::now();
  in->feedback(1,4,true,4);
  check(codec->snapshot().queued_frames==0,"Video codec missed a mute boundary between polls");
  const auto admissions=codec->snapshot().admitted;
  in->raw(1,170000,false,64,held_capture);
  check(codec->snapshot().admitted==admissions,"Video input held across mute was retagged as fresh");
  in->feedback(2);
  check(codec->snapshot().queued_frames==0,"New epoch retained old raw input");
  const auto dropped=codec->snapshot().dropped;
  in->raw(1,900000); in->raw(2,3001,true);
  in->raw(2,3001,false,std::numeric_limits<int>::max());
  check(codec->snapshot().dropped==dropped+3,"Stale/invalid raw media accepted");
  out->unblock(); in->raw(2,6002);
  eventually([&] { std::lock_guard lock(out->mutex); return out->encoded.size()>=2; },"Encoder did not recover after overload/epoch change");
  codec->quiesce(); graph.stop(); codec->throw_if_failed();
  { std::lock_guard lock(out->mutex);
    check(out->encoded.back().epoch==2 && out->encoded.back().timestamp==6002 && out->encoded.back().keyframe,"Codec lost epoch/timestamp or recovery keyframe");
  }
  const auto count=codec->snapshot().admitted; in->raw(2,9003);
  check(codec->snapshot().admitted==count,"Stopped codec admitted raw input");
  graph.release();
}
void decoder_recovery() {
  auto in=std::make_shared<Input>(); auto out=std::make_shared<Output>();
  auto codec=std::make_shared<nodes::VideoCodecNode>("decoder",video(),false);
  graphflow::engine::GraphEngine graph{1}; using G=engine::TrackGraphSpec;
  check(graph.loadFromNodes({out,in,codec},{G::connect<nodes::EncodedFrameEvent>(in,codec),G::connect<nodes::VideoFrameEvent>(codec,out),G::connect<nodes::KeyframeRequestEvent>(codec,in)}) && graph.init() && graph.start(),"Decoder graph start failed");
  in->encoded(1,3001,false); check(in->plis==1,"Decoder did not request initial keyframe");
  in->encoded(1,6002);
  eventually([&] { std::lock_guard lock(out->mutex); return !out->raw.empty(); },"Decoder did not deliver pixels");
  in->encoded(2,9003,false); check(in->plis==2,"Decoder epoch did not reset dependency chain");
  in->encoded(2,12004);
  eventually([&] { std::lock_guard lock(out->mutex); return out->raw.size()==2; },"Decoder epoch recovery failed");
  codec->quiesce(); graph.stop(); graph.release(); codec->throw_if_failed();
  std::lock_guard lock(out->mutex);
  const auto& frame=out->raw.back();
  check(frame.epoch==2 && frame.timestamp==12004 && frame.pixels->width==64 && frame.pixels->height==48 && frame.pixels->y.size()==64*48,"Decoded frame lost timing/owned pixels");
}
void decoded_output_bound() {
  auto in=std::make_shared<Input>(); auto out=std::make_shared<Output>();
  VideoCodecOptions options; options.max_bytes=2000;
  auto codec=std::make_shared<nodes::VideoCodecNode>("decoder",video(),false,options);
  graphflow::engine::GraphEngine graph{1}; using G=engine::TrackGraphSpec;
  check(graph.loadFromNodes({out,codec,in},{G::connect<nodes::EncodedFrameEvent>(in,codec),G::connect<nodes::VideoFrameEvent>(codec,out)}) && graph.init() && graph.start(),"Bounded decoder start failed");
  in->encoded(1,3001);
  eventually([&] { return !codec->snapshot().error.empty(); },"Oversized decoded pixels were not rejected");
  codec->quiesce(); graph.stop(); graph.release();
  std::lock_guard lock(out->mutex); check(out->raw.empty(),"Oversized pixels escaped decoder output budget");
}
}
int main() { try { encoder_queue(); decoder_recovery(); decoded_output_bound(); std::cout<<"Video graph codec contracts passed\n"; return 0; }
catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; } }
