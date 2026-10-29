#include "media_nodes/playout/audio_output_driver.hpp"
#include "media_nodes/playout/audio_output_transport.hpp"
#include "livekit/rtc/stream_pipeline.hpp"
#include "livekit/rtc/recording_node.hpp"
#include <fstream>
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>
using namespace livekit::sdk::rtc;
using namespace std::chrono_literals;
namespace {
void check(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
class FakeOutput final : public detail::AudioOutputDriver {
 public:
  std::mutex mutex;
  Render render;
  Failure failure;
  int starts=0,stops=0;
  bool fail_start=false;
  void start(const AudioOutputOptions&,Render next,Failure error) override {
    std::lock_guard lock(mutex); render=std::move(next); failure=std::move(error); ++starts;
    if(fail_start)throw std::runtime_error("Injected output startup failure");
  }
  void stop() noexcept override { std::lock_guard lock(mutex); if(render)++stops; render={}; failure={}; }
  std::array<int16_t,960> pull() {
    std::lock_guard lock(mutex); std::array<int16_t,960> result{}; if(render)render(result); return result;
  }
};
struct Fixture {
  FakeOutput* device;
  std::shared_ptr<AudioOutputSession> session;
  Fixture(std::chrono::milliseconds buffer=0ms,size_t max_tracks=4) {
    auto driver=std::make_unique<FakeOutput>(); device=driver.get();
    AudioOutputOptions options; options.buffer=buffer; options.max_latency=120ms; options.max_tracks=max_tracks;
    session=detail::AudioOutputFactory::create(options,std::move(driver));
  }
};
nodes::AudioFrameEvent pcm(uint64_t timestamp,int16_t value,size_t frames=480,Epoch epoch=1) {
  nodes::AudioFrameEvent event; event.timestamp=timestamp; event.epoch=epoch;
  auto samples=std::make_shared<PcmAudioFrame>(); samples->samples.assign(frames,value); event.pcm=std::move(samples); return event;
}
bool all(const std::array<int16_t,960>& samples,int16_t value) { return std::all_of(samples.begin(),samples.end(),[&](auto v) { return v==value; }); }
void independent_tracks() {
  Fixture f(0ms,2); auto a=f.session->open_track("a"),b=f.session->open_track("b");
  check(f.device->starts==1 && f.session->snapshot().tracks==2,"Tracks opened separate output devices");
  bool duplicate=false,excess=false;
  try { f.session->open_track("a"); } catch(const std::invalid_argument&) { duplicate=true; }
  try { f.session->open_track("c"); } catch(const std::invalid_argument&) { excess=true; }
  check(duplicate && excess && f.session->snapshot().tracks==2,"Output track admission limit failed");
  a->write(pcm(0,20000,960)); b->write(pcm(1234,20000,960));
  check(all(f.device->pull(),32767),"Mixing wrapped instead of saturating");
  a->close(); check(f.device->stops==0 && f.session->snapshot().running,"Closing one track stopped shared device");
  check(all(f.device->pull(),20000),"Closing one track cleared the other's pending PCM");
  auto replacement=f.session->open_track("a");
  check(!a->write(pcm(1000,9)) && a->snapshot().closed,"Closed output handle admitted media");
  replacement->write(pcm(0,1000)); replacement->set_muted(true);
  b->set_gain(2); b->write(pcm(2194,-20000));
  check(all(f.device->pull(),-32768),"Per-track mute/gain or negative saturation failed");
  auto old=pcm(480,11000); replacement->set_muted(false);
  check(!replacement->write(old),"Unmute admitted PCM captured before transition");
  replacement->write(pcm(960,3000));
  check(all(f.device->pull(),3000),"Unmuted track failed to reanchor");
  b->close(); replacement->close();
  check(f.device->stops==1 && !f.session->snapshot().running,"Last track did not stop device");
  auto again=f.session->open_track("b"); again->write(pcm(0,123));
  check(f.device->starts==2 && all(f.device->pull(),123),"Output device could not restart independently");
}
void timing() {
  Fixture f(20ms); auto track=f.session->open_track("timed");
  check(track->write(pcm(1000,1000)) && track->write(pcm(1960,2000)),"Timestamped PCM rejected");
  check(all(f.device->pull(),0) && all(f.device->pull(),0),"Prebuffer was not honored");
  check(all(f.device->pull(),1000) && all(f.device->pull(),0) && all(f.device->pull(),2000),"Timestamp gap compressed or stretched");
  check(!track->write(pcm(1000,9000)),"Overlapping PCM replayed");
  track->write(pcm(2440,3000));
  check(all(f.device->pull(),3000),"Continuous timestamp failed after a gap");
  track->write(pcm(2920,4000));
  track->write(pcm(0,7000,480,2));
  check(!track->write(pcm(3400,9000,480,1)),"Old epoch entered output queue");
  check(all(f.device->pull(),0) && all(f.device->pull(),0) && all(f.device->pull(),7000),"Epoch reset replayed stale PCM");
  auto stale=pcm(480,12000,480,2); stale.captured=SteadyClock::now()-1s;
  check(!track->write(stale),"Expired decoded PCM admitted");
  track->write(pcm(std::numeric_limits<uint64_t>::max()-6000,6000,480,2));
  check(all(f.device->pull(),0) && all(f.device->pull(),0) && all(f.device->pull(),6000),"Large timestamp jump overflowed or retained unbounded silence");
  check(!track->write(pcm(std::numeric_limits<uint64_t>::max()-10,1,480,2)),"Overflowing timestamp admitted");
}
void queue_and_short_packets() {
  Fixture f; auto track=f.session->open_track("queue");
  for(int i=0;i<13;++i)check(track->write(pcm(i*480,int16_t(i+1))),"Bounded output write failed");
  check(track->snapshot().queued_samples==480 && track->snapshot().dropped==12 && all(f.device->pull(),13),"Overflow did not replace stale backlog with recent media");
  track->close(); auto short_track=f.session->open_track("short");
  for(int i=0;i<4;++i)short_track->write(pcm(i*120,int16_t(1000+i),120));
  const auto rendered=f.device->pull();
  for(size_t i=0;i<960;++i)check(rendered[i]==1000+i/240,"2.5 ms packets were rounded to device blocks");
  check(short_track->snapshot().queued_samples==0 && short_track->snapshot().rendered_samples==480,"Partial packet accounting failed");
  auto stereo=std::make_shared<PcmAudioFrame>(); stereo->channels=2; stereo->samples.resize(960);
  for(size_t i=0;i<480;++i) { stereo->samples[i*2]=123; stereo->samples[i*2+1]=-456; }
  auto event=pcm(480,0); event.pcm=stereo; short_track->write(event);
  const auto routed=f.device->pull();
  check(routed[0]==123 && routed[1]==-456 && routed[958]==123 && routed[959]==-456,"Stereo output routing failed");
}
void failures_and_barrier() {
  Fixture f; f.device->fail_start=true;
  bool failed=false; try { f.session->open_track("failed"); } catch(const std::runtime_error&) { failed=true; }
  check(failed && f.device->stops==1 && f.session->snapshot().tracks==0,"Output startup rollback failed");
  f.device->fail_start=false; auto track=f.session->open_track("retry");
  std::jthread render([&](std::stop_token stop) { while(!stop.stop_requested()) { f.device->pull(); std::this_thread::yield(); } });
  for(int i=0;i<100;++i)track->write(pcm(i*480,1000));
  track->close(); render.request_stop(); render.join();
  check(all(f.device->pull(),0) && !track->write(pcm(100000,1)) && f.device->stops==2,"Callback barrier failed on concurrent close");
  auto fresh=f.session->open_track("error");
  { std::lock_guard lock(f.device->mutex); f.device->failure("Injected device failure"); }
  bool surfaced=false; try { f.session->throw_if_failed(); } catch(const std::runtime_error&) { surfaced=true; }
  check(surfaced && !fresh->write(pcm(0,123)),"Device failure was hidden from graph polling");
}
class Source : public graphflow::core::GraphNode {
 public:
  Source():GraphNode("source",nullptr) {}
  void push(const nodes::AudioFrameEvent& frame) { publish(frame); }
  void push(const nodes::VideoFrameEvent& frame) { publish(frame); }
};
void graph_sink() {
  Fixture f;
  auto source=std::make_shared<Source>();
  auto output=std::make_shared<nodes::AudioOutputNode>("output","track",f.session);
  graphflow::engine::GraphEngine graph{1};
  check(graph.loadFromNodes({output,source},{engine::TrackGraphSpec::connect<nodes::AudioFrameEvent>(source,output)}) && graph.init() && graph.start(),"Output graph startup failed");
  source->push(pcm(0,4000)); check(all(f.device->pull(),4000),"Graph output sink did not enqueue decoded PCM");
  output->quiesce(); source->push(pcm(480,5000));
  check(output->snapshot().closed && all(f.device->pull(),0),"Graph quiescence allowed late decoded audio");
  graph.stop(); graph.release();
}
void native_callback() {
  int pulls=0; std::string error;
  auto transport=detail::create_audio_output_transport([&](auto samples) {
    check(samples.size()==960,"Device converter requested incomplete mixer block"); ++pulls;
    for(size_t i=0;i<480;++i) { samples[i*2]=12000; samples[i*2+1]=-4000; }
  },[&](auto failure) { error=std::move(failure); });
  std::array<int16_t,960> stereo{}; size_t out=0; int64_t elapsed=99,ntp=99;
  check(transport->NeedMorePlayData(480,4,2,48000,stereo.data(),out,&elapsed,&ntp)==0 && out==960 && elapsed==-1 && ntp==-1,
    "Native callback did not return total interleaved sample count and initialized timestamps");
  check(stereo[0]==12000 && stereo[1]==-4000 && stereo[958]==12000 && stereo[959]==-4000,"Native callback stereo routing failed");
  std::vector<int16_t> converted(441);
  for(int i=0;i<3;++i)check(transport->NeedMorePlayData(441,2,1,44100,converted.data(),out,&elapsed,&ntp)==0 && out==441,"Native output 44.1 kHz conversion failed");
  check(std::all_of(converted.begin()+100,converted.end(),[](auto v) { return v>3900 && v<4100; }),"Native output downmix/resampler did not preserve DC level");
  // Format changes reset the native resampler, including returning from direct PCM.
  transport->NeedMorePlayData(480,4,2,48000,stereo.data(),out,&elapsed,&ntp);
  std::vector<int16_t> low_rate(160*2);
  check(transport->NeedMorePlayData(160,4,2,16000,low_rate.data(),out,&elapsed,&ntp)==0 && out==320,"Native output changed-rate callback failed");
  check(low_rate[300]>11000 && low_rate[301]<-3500,"Native output resampling lost stereo separation");
  transport->PullRenderData(16,48000,2,480,stereo.data(),&elapsed,&ntp);
  check(stereo[0]==12000 && stereo[1]==-4000,"PullRenderData bypassed device converter");
  const auto before=pulls;
  std::fill(stereo.begin(),stereo.end(),99);
  check(transport->NeedMorePlayData(480,4,2,44100,stereo.data(),out,&elapsed,&ntp)!=0 && !error.empty() && pulls==before && all(stereo,0),
    "Invalid native format escaped callback or leaked uninitialized audio");
}
void recording() {
  const auto directory=std::filesystem::temp_directory_path()/("rtc-recording-"+std::to_string(SteadyClock::now().time_since_epoch().count()));
  std::filesystem::create_directory(directory);
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; for(const auto& file:std::filesystem::directory_iterator(path))std::filesystem::remove(file.path(),error); std::filesystem::remove(path,error); } } cleanup{directory};
  const auto read=[&](const char* name) { std::ifstream input(directory/name,std::ios::binary); return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),{}); };
  const auto u32=[](const auto& bytes,size_t at) { return uint32_t(bytes.at(at))|uint32_t(bytes.at(at+1))<<8|uint32_t(bytes.at(at+2))<<16|uint32_t(bytes.at(at+3))<<24; };
  auto source=std::make_shared<Source>();
  auto audio=std::make_shared<nodes::RecordingNode>("audio",directory,"audio",MediaKind::audio);
  auto video=std::make_shared<nodes::RecordingNode>("video",directory,"video",MediaKind::video);
  graphflow::engine::GraphEngine graph{1};
  check(graph.loadFromNodes({audio,video,source},{engine::TrackGraphSpec::connect<nodes::AudioFrameEvent>(source,audio),
    engine::TrackGraphSpec::connect<nodes::VideoFrameEvent>(source,video)}) && graph.init() && graph.start(),"Recording graph failed");
  source->push(pcm(0,-1234,960)); source->push(pcm(960,2345,480)); source->push(pcm(0,3456,480,2));
  auto pixels=std::make_shared<livekit::sdk::VideoFrame>(); pixels->width=pixels->height=3;
  pixels->stride_y=4; pixels->stride_u=pixels->stride_v=2; pixels->y.assign(12,55); pixels->u.assign(4,99); pixels->v.assign(4,122);
  nodes::VideoFrameEvent frame; frame.pixels=pixels; frame.epoch=1; source->push(frame);
  audio->quiesce(); video->quiesce(); graph.stop(); graph.release(); audio->throw_if_failed(); video->throw_if_failed();
  const auto wav=read("audio-0.wav"),next=read("audio-1.wav"),y4m=read("video-0.y4m");
  check(wav.size()==44+1440*2 && u32(wav,4)==wav.size()-8 && u32(wav,40)==1440*2 && u32(wav,24)==48000,"Recording did not finalize WAV length/rate");
  check(wav[44]==uint8_t(uint16_t(-1234)) && wav[45]==uint8_t(uint16_t(-1234)>>8) && next.size()==44+480*2,"Recording changed PCM or merged epochs");
  const std::string header="YUV4MPEG2 W3 H3 F30:1 Ip A1:1 C420jpeg\nFRAME\n";
  check(y4m.size()==header.size()+17 && std::equal(header.begin(),header.end(),y4m.begin()),"Recording Y4M layout/stride invalid");
  check(y4m[header.size()+8]==55 && y4m[header.size()+9]==99 && y4m.back()==122,"Recording leaked I420 row padding");
  auto collision=std::make_shared<nodes::RecordingNode>("collision",directory,"audio",MediaKind::audio);
  auto second=std::make_shared<Source>(); graphflow::engine::GraphEngine duplicate{1};
  check(duplicate.loadFromNodes({collision,second},{engine::TrackGraphSpec::connect<nodes::AudioFrameEvent>(second,collision)}) && duplicate.init() && duplicate.start(),"Collision graph failed before recording");
  second->push(pcm(0,0)); collision->quiesce(); duplicate.stop(); duplicate.release();
  check(!collision->snapshot().error.empty() && read("audio-0.wav")==wav,"Recording overwrote an existing file");
}
}
int main() {
  try { independent_tracks(); timing(); queue_and_short_packets(); failures_and_barrier(); graph_sink(); native_callback(); recording();
    std::cout<<"RTC shared audio output contracts passed\n";
  } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
