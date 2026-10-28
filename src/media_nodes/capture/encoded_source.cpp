#include "media_nodes/capture/encoded_source.hpp"
#include "livekit/error.hpp"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace livekit::sdk {
struct EncodedMediaSource::Impl {
  EncodedSourceOptions options;mutable std::mutex mutex;std::condition_variable_any ready;
  EncodedMediaStats stats;std::deque<EncodedMediaFrame> frames;uint64_t lease=0;bool bound=false;
  std::optional<std::chrono::steady_clock::time_point> last;
  void clear(){stats.dropped+=frames.size();frames.clear();stats.queued_bytes=0;stats.queued_frames=0;stats.keyframe_required=options.codec!="opus";}
  void expire(){if(!frames.empty()&&std::chrono::steady_clock::now()-frames.front().captured>options.max_age)clear();}
};
EncodedMediaSource::EncodedMediaSource(EncodedSourceOptions options):impl_(std::make_unique<Impl>()){
  if(options.codec!="h264"&&options.codec!="h265"&&options.codec!="vp8"&&options.codec!="opus")throw std::invalid_argument("Unsupported encoded codec");
  if(options.width<2||options.height<2||options.width>3840||options.height>2160||!options.max_frames||options.max_frames>1024||options.max_bytes<65536||options.max_bytes>128*1024*1024||options.max_age.count()<1||options.max_age>std::chrono::seconds(5))throw std::invalid_argument("Invalid encoded source bounds");
  impl_->options=std::move(options);impl_->stats.keyframe_required=impl_->options.codec!="opus";
}
EncodedMediaSource::~EncodedMediaSource()=default;
EncodedSourceOptions EncodedMediaSource::options()const{return impl_->options;}
EncodedMediaStats EncodedMediaSource::stats()const{std::lock_guard lock(impl_->mutex);return impl_->stats;}
void EncodedMediaSource::request_keyframe(){auto& s=*impl_;std::lock_guard lock(s.mutex);if(s.options.codec!="opus")s.clear();s.ready.notify_all();}
void EncodedMediaSource::close(){std::lock_guard lock(impl_->mutex);impl_->stats.closed=true;impl_->stats.accepting=false;impl_->clear();impl_->ready.notify_all();}
EncodedWriteStatus EncodedMediaSource::write(EncodedMediaFrame frame){
  auto& s=*impl_;std::lock_guard lock(s.mutex);if(s.stats.closed)return EncodedWriteStatus::closed;
  const auto now=std::chrono::steady_clock::now();if(frame.captured==std::chrono::steady_clock::time_point{})frame.captured=now;
  if(frame.codec.empty())frame.codec=s.options.codec;
  if(frame.codec!=s.options.codec||frame.data.empty()||frame.data.size()>s.options.max_bytes||frame.duration.count()<1||frame.duration>std::chrono::seconds(1)||frame.captured>now+std::chrono::seconds(1)||(s.last&&frame.captured<=*s.last))throw std::invalid_argument("Invalid encoded frame format or timestamp");
  if(frame.codec!="opus"){
    if(frame.width==0)frame.width=s.options.width;if(frame.height==0)frame.height=s.options.height;
    if(frame.width!=s.options.width||frame.height!=s.options.height)throw std::invalid_argument("Encoded frame dimensions changed");
    if(frame.codec=="h264"||frame.codec=="h265"){
      if(frame.data.size()<5||frame.data[0]!=0||frame.data[1]!=0||!(frame.data[2]==1||(frame.data[2]==0&&frame.data[3]==1)))throw std::invalid_argument("Encoded H26x requires Annex-B access units");
    }
  }
  s.expire();if(!s.stats.accepting||now-frame.captured>s.options.max_age||s.frames.size()>=s.options.max_frames||s.stats.queued_bytes+frame.data.size()>s.options.max_bytes)return EncodedWriteStatus::backpressure;
  if(s.stats.keyframe_required&&!frame.keyframe&&frame.codec!="opus")return EncodedWriteStatus::keyframe_required;
  if(frame.keyframe)s.stats.keyframe_required=false;s.last=frame.captured;frame.generation=s.stats.generation;
  s.stats.queued_bytes+=frame.data.size();s.frames.push_back(std::move(frame));s.stats.queued_frames=s.frames.size();++s.stats.accepted;s.ready.notify_all();return EncodedWriteStatus::accepted;
}
struct EncodedMediaStream::Impl {
  mutable std::mutex mutex;std::condition_variable_any ready;std::deque<std::shared_ptr<const EncodedMediaFrame>> frames;
  size_t max_frames,max_bytes;EncodedMediaStats stats;bool bound=false;
};
EncodedMediaStream::EncodedMediaStream(size_t frames,size_t bytes):impl_(std::make_unique<Impl>()){
  if(!frames||frames>4096||bytes<65536||bytes>256*1024*1024)throw std::invalid_argument("Invalid encoded stream bounds");impl_->max_frames=frames;impl_->max_bytes=bytes;
}
EncodedMediaStream::~EncodedMediaStream()=default;
EncodedMediaStats EncodedMediaStream::stats()const{std::lock_guard lock(impl_->mutex);return impl_->stats;}
void EncodedMediaStream::close(){std::lock_guard lock(impl_->mutex);impl_->stats.closed=true;impl_->stats.accepting=false;impl_->frames.clear();impl_->stats.queued_bytes=0;impl_->stats.queued_frames=0;impl_->ready.notify_all();}
std::shared_ptr<const EncodedMediaFrame> EncodedMediaStream::read(std::chrono::milliseconds timeout,std::stop_token stop){
  if(timeout.count()<0)throw std::invalid_argument("Negative read timeout");auto& s=*impl_;std::unique_lock lock(s.mutex);
  const auto ready=s.ready.wait_for(lock,stop,timeout,[&]{return s.stats.closed||!s.frames.empty();});
  if(stop.stop_requested())throw SDKError(ErrorDomain::media,ErrorCode::cancelled,"Encoded read cancelled");if(!ready)throw SDKError(ErrorDomain::media,ErrorCode::timeout,"Encoded read timeout");if(s.frames.empty())return {};
  auto frame=s.frames.front();s.frames.pop_front();s.stats.queued_bytes-=frame->data.size();s.stats.queued_frames=s.frames.size();++s.stats.consumed;return frame;
}
namespace detail {
EncodedSourceReader::EncodedSourceReader(std::shared_ptr<EncodedMediaSource> source):source_(std::move(source)){
  if(!source_)throw std::invalid_argument("Null encoded source");auto& s=*source_->impl_;std::lock_guard lock(s.mutex);
  if(s.bound||s.stats.closed)throw std::logic_error("Encoded source already bound or closed");s.bound=true;lease_=++s.lease;
}
EncodedSourceReader::~EncodedSourceReader(){stop();auto& s=*source_->impl_;std::lock_guard lock(s.mutex);if(s.lease==lease_)s.bound=false;}
void EncodedSourceReader::start(){auto& s=*source_->impl_;std::lock_guard lock(s.mutex);if(s.stats.closed)return;s.clear();s.last.reset();++s.stats.generation;s.stats.accepting=true;}
void EncodedSourceReader::stop(){auto& s=*source_->impl_;std::lock_guard lock(s.mutex);s.stats.accepting=false;s.clear();s.ready.notify_all();}
std::optional<EncodedMediaFrame> EncodedSourceReader::read(std::stop_token stop,bool wait){
  auto& s=*source_->impl_;std::unique_lock lock(s.mutex);
  if(wait)s.ready.wait(lock,stop,[&]{return !s.stats.accepting||!s.frames.empty();});
  if(stop.stop_requested()||!s.stats.accepting||s.lease!=lease_)return {};s.expire();if(s.frames.empty())return {};
  if(s.frames.front().captured>std::chrono::steady_clock::now()){
    if(!wait)return {};const auto time=s.frames.front().captured;
    s.ready.wait_until(lock,stop,time,[&]{return !s.stats.accepting;});if(stop.stop_requested()||!s.stats.accepting||s.frames.empty())return {};
  }
  auto frame=std::move(s.frames.front());s.frames.pop_front();s.stats.queued_bytes-=frame.data.size();s.stats.queued_frames=s.frames.size();++s.stats.consumed;return frame;
}
void EncodedSourceReader::feedback(bool keyframe,uint32_t bitrate){auto& s=*source_->impl_;std::lock_guard lock(s.mutex);if(s.lease!=lease_)return;if(keyframe&&s.options.codec!="opus")s.clear();if(bitrate)s.stats.target_bitrate=bitrate;}
EncodedOutput::EncodedOutput(std::shared_ptr<EncodedMediaStream> stream):stream_(std::move(stream)){auto& s=*stream_->impl_;std::lock_guard lock(s.mutex);if(s.bound||s.stats.closed)throw std::logic_error("Encoded stream already bound or closed");s.bound=true;s.stats.accepting=true;++s.stats.generation;}
EncodedOutput::~EncodedOutput(){close();}
uint64_t EncodedOutput::generation()const{std::lock_guard lock(stream_->impl_->mutex);return stream_->impl_->stats.generation;}
uint64_t EncodedOutput::advance(){auto& s=*stream_->impl_;std::lock_guard lock(s.mutex);s.frames.clear();s.stats.queued_bytes=0;s.stats.queued_frames=0;return ++s.stats.generation;}
void EncodedOutput::close(){stream_->close();}
void EncodedOutput::push(EncodedMediaFrame frame,uint64_t generation){
  auto& s=*stream_->impl_;std::lock_guard lock(s.mutex);if(s.stats.closed||generation!=s.stats.generation||frame.data.size()>s.max_bytes){++s.stats.dropped;return;}
  while(!s.frames.empty()&&(s.frames.size()>=s.max_frames||s.stats.queued_bytes+frame.data.size()>s.max_bytes)){s.stats.queued_bytes-=s.frames.front()->data.size();s.frames.pop_front();++s.stats.dropped;}
  frame.generation=generation;s.stats.queued_bytes+=frame.data.size();s.frames.push_back(std::make_shared<const EncodedMediaFrame>(std::move(frame)));s.stats.queued_frames=s.frames.size();++s.stats.accepted;s.ready.notify_all();
}
}
}
