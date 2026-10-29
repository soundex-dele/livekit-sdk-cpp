#include "livekit/rtc/recording_node.hpp"
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <variant>

namespace livekit::sdk::rtc::nodes {
namespace {
void le(std::ostream& out,uint32_t value,int count) { for(int i=0;i<count;++i)out.put(char(value>>(i*8))); }
}
struct RecordingNode::Impl {
  struct Item { std::variant<AudioFrameEvent,VideoFrameEvent> frame; size_t bytes; };
  const std::filesystem::path directory;
  const std::string id;
  const MediaKind kind;
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::deque<Item> queue;
  std::thread worker;
  RecordingSnapshot stats;
  bool active=false,closed=false;
  std::ofstream file;
  uint64_t bytes=0;
  Epoch epoch=0;
  uint32_t width=0,height=0,rate=0,channels=0,part=0;
  Impl(std::filesystem::path path,TrackId id,MediaKind kind) : directory(std::move(path)),id(std::move(id)),kind(kind) {
    if(directory.empty() || this->id.empty())throw std::invalid_argument("Recording needs directory and track ID");
  }
  void fail(std::string message) {
    std::lock_guard lock(mutex); if(stats.error.empty())stats.error=std::move(message);
    closed=true; queue.clear(); stats.queued_bytes=stats.queued_frames=0; changed.notify_all();
  }
  void enqueue(Item item) {
    std::lock_guard lock(mutex); if(!active || closed)return;
    if(queue.size()>=32 || item.bytes>64*1024*1024-stats.queued_bytes) {
      stats.error="Recording queue overflow"; closed=true; changed.notify_all(); return;
    }
    stats.queued_bytes+=item.bytes; queue.push_back(std::move(item)); stats.queued_frames=queue.size(); ++stats.admitted;
    changed.notify_one();
  }
  void finish() {
    if(!file.is_open())return;
    if(kind==MediaKind::audio) { file.seekp(4); le(file,uint32_t(bytes+36),4); file.seekp(40); le(file,uint32_t(bytes),4); }
    file.flush(); if(!file)throw std::runtime_error("Recording finalization failed"); file.close();
  }
  void open(const char* extension) {
    finish(); bytes=0;
    std::string safe;
    for(unsigned char c:id)safe+=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_' ? char(c) : '_';
    auto path=directory/(safe+"-"+std::to_string(part++)+extension);
    // Reserve the exact path exclusively so parallel writers cannot both pass
    // an exists() check and truncate each other's recording.
    std::FILE* reserved=nullptr;
#ifdef _WIN32
    _wfopen_s(&reserved,path.c_str(),L"wbx");
#else
    reserved=std::fopen(path.c_str(),"wbx");
#endif
    if(!reserved)throw std::runtime_error("Cannot reserve recording path (already exists or inaccessible)");
    std::fclose(reserved);
    file.open(path,std::ios::binary); if(!file)throw std::runtime_error("Cannot open recording file");
  }
  void write(const AudioFrameEvent& event) {
    const auto& pcm=*event.pcm; const auto count=pcm.samples.size()*2;
    if(!file.is_open() || epoch!=event.epoch || rate!=pcm.sample_rate || channels!=pcm.channels || bytes+count>0xffff0000ULL) {
      open(".wav"); epoch=event.epoch; rate=pcm.sample_rate; channels=pcm.channels;
      file<<"RIFF"; le(file,36,4); file<<"WAVEfmt "; le(file,16,4); le(file,1,2); le(file,channels,2);
      le(file,rate,4); le(file,rate*channels*2,4); le(file,channels*2,2); le(file,16,2); file<<"data"; le(file,0,4);
    }
    for(auto sample:pcm.samples)le(file,uint16_t(sample),2);
    bytes+=count;
  }
  void write(const VideoFrameEvent& event) {
    const auto& pixels=*event.pixels;
    if(!file.is_open() || epoch!=event.epoch || width!=pixels.width || height!=pixels.height) {
      open(".y4m"); epoch=event.epoch; width=pixels.width; height=pixels.height;
      file<<"YUV4MPEG2 W"<<width<<" H"<<height<<" F30:1 Ip A1:1 C420jpeg\n";
    }
    file<<"FRAME\n";
    const auto plane=[&](const auto& data,int stride,int w,int h) {
      for(int y=0;y<h;++y)file.write(reinterpret_cast<const char*>(data.data()+size_t(y)*stride),w);
    };
    plane(pixels.y,pixels.stride_y,pixels.width,pixels.height);
    plane(pixels.u,pixels.stride_u,(pixels.width+1)/2,(pixels.height+1)/2);
    plane(pixels.v,pixels.stride_v,(pixels.width+1)/2,(pixels.height+1)/2);
  }
  void run() {
    try {
      while(true) {
        Item item;
        {
          std::unique_lock lock(mutex); changed.wait(lock,[&] { return closed || !queue.empty(); });
          if(queue.empty())break;
          item=std::move(queue.front()); queue.pop_front(); stats.queued_bytes-=item.bytes; stats.queued_frames=queue.size();
        }
        std::visit([&](const auto& frame) { write(frame); },item.frame);
        if(!file)throw std::runtime_error("Recording write failed");
        std::lock_guard lock(mutex); ++stats.written;
      }
      finish();
    } catch(const std::exception& e) { fail(e.what()); try { finish(); } catch(...) {} }
    catch(...) { fail("Unknown recording failure"); }
  }
};
RecordingNode::RecordingNode(std::string name,std::filesystem::path directory,TrackId id,MediaKind kind)
  : GraphNode(name,nullptr),impl_(std::make_unique<Impl>(std::move(directory),std::move(id),kind)) {}
RecordingNode::~RecordingNode() { quiesce(); release(); }
bool RecordingNode::doInit() {
  if(impl_->kind==MediaKind::audio)subscribe<AudioFrameEvent>([this](const auto& event) {
    if(!event.pcm || !event.epoch)return;
    const auto& pcm=*event.pcm;
    if(pcm.format!=PcmFormat::s16 || !pcm.float_samples.empty() || (pcm.channels!=1&&pcm.channels!=2) ||
       pcm.sample_rate!=48000 || pcm.samples.empty() || pcm.samples.size()%pcm.channels || pcm.samples_per_channel()>5760) {
      impl_->fail("Invalid recording PCM"); return;
    }
    impl_->enqueue({event,pcm.samples.size()*2});
  });
  else subscribe<VideoFrameEvent>([this](const auto& event) {
    if(!event.pixels || !event.epoch)return;
    const auto& p=*event.pixels;
    const auto plane=[](const auto& bytes,int stride,int w,int h) { return stride>=w && bytes.size()>=size_t(stride)*(h-1)+w; };
    if(p.width<=0 || p.height<=0 || p.width>8192 || p.height>8192 ||
       !plane(p.y,p.stride_y,p.width,p.height) || !plane(p.u,p.stride_u,(p.width+1)/2,(p.height+1)/2) ||
       !plane(p.v,p.stride_v,(p.width+1)/2,(p.height+1)/2)) { impl_->fail("Invalid recording I420"); return; }
    impl_->enqueue({event,p.y.size()+p.u.size()+p.v.size()});
  });
  return true;
}
bool RecordingNode::doStart() {
  std::lock_guard lock(impl_->mutex); if(impl_->closed)return false;
  std::filesystem::create_directories(impl_->directory); impl_->active=true;
  impl_->worker=std::thread([this] { impl_->run(); }); return true;
}
void RecordingNode::quiesce() {
  { std::lock_guard lock(impl_->mutex); impl_->closed=true; impl_->active=false; impl_->changed.notify_all(); }
  if(impl_->worker.joinable())impl_->worker.join();
}
void RecordingNode::doStop() { quiesce(); }
RecordingSnapshot RecordingNode::snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->stats; }
void RecordingNode::throw_if_failed() const { const auto state=snapshot(); if(!state.error.empty())throw std::runtime_error(state.error); }
}
