#include "livekit/rtp.hpp"
#include "livekit/error.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <algorithm>

namespace livekit::sdk {
struct PacketStream::Impl {explicit Impl(size_t n):capacity(n){}mutable std::mutex mutex;std::condition_variable_any ready;size_t capacity;std::deque<PacketEvent> queue;uint64_t dropped=0;bool closed=false;};
PacketStream::PacketStream(size_t capacity){if(!capacity||capacity>4096)throw std::invalid_argument("Invalid packet observer capacity");impl_=std::make_unique<Impl>(capacity);}
PacketStream::~PacketStream()=default;
void PacketStream::push(PacketEvent event){auto& s=*impl_;std::lock_guard lock(s.mutex);if(s.closed)return;if(s.queue.size()==s.capacity){s.queue.pop_front();++s.dropped;}s.queue.push_back(std::move(event));s.ready.notify_one();}
void PacketStream::close(){auto& s=*impl_;std::lock_guard lock(s.mutex);s.closed=true;s.queue.clear();s.ready.notify_all();}
uint64_t PacketStream::dropped()const{std::lock_guard lock(impl_->mutex);return impl_->dropped;}
std::optional<PacketEvent> PacketStream::read(std::chrono::milliseconds timeout,std::stop_token stop){
  if(timeout.count()<0)throw std::invalid_argument("Negative packet read timeout");auto& s=*impl_;std::unique_lock lock(s.mutex);
  bool ready=s.ready.wait_for(lock,stop,timeout,[&]{return s.closed||!s.queue.empty();});
  if(stop.stop_requested())throw SDKError(ErrorDomain::media,ErrorCode::cancelled,"Packet read cancelled");
  if(!ready)throw SDKError(ErrorDomain::media,ErrorCode::timeout,"Packet read timeout");if(s.queue.empty())return {};
  auto event=std::move(s.queue.front());s.queue.pop_front();return event;
}
RtpClock::RtpClock(uint32_t rate):rate_(rate){if(!rate||rate>1000000)throw std::invalid_argument("Invalid RTP clock rate");}
void RtpClock::sender_report(uint32_t rtp,uint64_t ntp){rtp_=rtp;ntp_=std::chrono::nanoseconds(int64_t(ntp>>32)*1000000000+int64_t((ntp&0xffffffff)*1000000000>>32));}
std::optional<std::chrono::nanoseconds> RtpClock::ntp_time(uint32_t rtp)const{if(!ntp_)return {};return *ntp_+std::chrono::nanoseconds(int64_t(int32_t(rtp-rtp_))*1000000000/rate_);}
void RtpClock::reset(){ntp_.reset();}
namespace {
uint16_t read16(std::span<const uint8_t> p,size_t n){return uint16_t(p[n])<<8|p[n+1];}
uint32_t read32(std::span<const uint8_t> p,size_t n){return uint32_t(read16(p,n))<<16|read16(p,n+2);}
}
struct RtpMediaSource::Impl {
  std::mutex mutex;std::shared_ptr<EncodedMediaSource> source;EncodedSourceOptions options;uint8_t pt;uint32_t ssrc;
  std::optional<uint16_t> next;std::optional<uint32_t> timestamp,base;std::chrono::steady_clock::time_point epoch;
  std::vector<uint8_t> unit;bool key=false,fragment=false,wait_key=true;uint64_t generation=0;
  void clear(){unit.clear();timestamp.reset();fragment=false;key=false;}
};
RtpMediaSource::RtpMediaSource(EncodedSourceOptions options,uint8_t pt,uint32_t ssrc):impl_(std::make_unique<Impl>()){
  if(pt>127||!ssrc)throw std::invalid_argument("Invalid RTP payload type/SSRC");auto& s=*impl_;s.source=std::make_shared<EncodedMediaSource>(options);s.options=std::move(options);s.pt=pt;s.ssrc=ssrc;
}
RtpMediaSource::~RtpMediaSource()=default;
std::shared_ptr<EncodedMediaSource> RtpMediaSource::source()const{return impl_->source;}
void RtpMediaSource::reset(){std::lock_guard lock(impl_->mutex);impl_->clear();impl_->next.reset();impl_->base.reset();}
EncodedWriteStatus RtpMediaSource::write(std::span<const uint8_t> p){
  auto& s=*impl_;std::lock_guard lock(s.mutex);
  const auto stats=s.source->stats();if(stats.generation!=s.generation){s.clear();s.next.reset();s.base.reset();s.generation=stats.generation;}
  if(stats.closed)return EncodedWriteStatus::closed;if(!stats.accepting)return EncodedWriteStatus::backpressure;
  if(p.size()<12||p.size()>65535||(p[0]>>6)!=2||(p[1]&127)!=s.pt||read32(p,8)!=s.ssrc)throw std::invalid_argument("Invalid RTP header or unmapped PT/SSRC");
  size_t offset=12+4*(p[0]&15),end=p.size();
  if(offset>end)throw std::invalid_argument("Truncated RTP CSRC");
  if(p[0]&16){if(offset+4>end)throw std::invalid_argument("Truncated RTP extension");offset+=4+4*size_t(read16(p,offset+2));}
  if(offset>end)throw std::invalid_argument("Truncated RTP extension data");
  if(p[0]&32){if(!p.back()||p.back()>end-offset)throw std::invalid_argument("Invalid RTP padding");end-=p.back();}
  if(offset>=end)throw std::invalid_argument("Empty RTP payload");
  const auto seq=read16(p,2);const auto ts=read32(p,4);bool gap=s.next&&seq!=*s.next;s.next=uint16_t(seq+1);
  if(gap||(s.timestamp&&*s.timestamp!=ts)){s.clear();s.wait_key=true;s.source->request_keyframe();}s.timestamp=ts;
  auto data=p.subspan(offset,end-offset);auto invalid=[&]()->EncodedWriteStatus{s.clear();return EncodedWriteStatus::keyframe_required;};
  auto annex=[&]{s.unit.insert(s.unit.end(),{0,0,0,1});};
  if(s.options.codec=="opus"){s.unit.assign(data.begin(),data.end());s.key=true;}
  else if(s.options.codec=="vp8"){
    size_t n=1;const bool start=(data[0]&16)&&!(data[0]&15);
    if(data[0]&128){if(n==data.size())return invalid();auto ext=data[n++];if(ext&128){if(n==data.size())return invalid();auto id=data[n++];if(id&128)++n;}if(ext&64)++n;if(ext&48)++n;}
    if(n>=data.size()||(!start&&s.unit.empty()))return invalid();
    if(start){s.unit.clear();s.key=!(data[n]&1);}s.unit.insert(s.unit.end(),data.begin()+n,data.end());
  }else{
    const bool hevc=s.options.codec=="h265";const auto type=hevc?(data[0]>>1)&63:data[0]&31;
    if(hevc&&data.size()<2)return invalid();
    const int aggregation=hevc?48:24,fu=hevc?49:28;
    auto nal=[&](std::span<const uint8_t> bytes){annex();s.unit.insert(s.unit.end(),bytes.begin(),bytes.end());auto t=hevc?(bytes[0]>>1)&63:bytes[0]&31;s.key|=hevc?(t>=16&&t<=21):t==5;};
    if(type==aggregation){size_t n=hevc?2:1;while(n<data.size()){if(n+2>data.size())return invalid();auto len=read16(data,n);n+=2;if(len<(hevc?2:1)||n+len>data.size())return invalid();nal(data.subspan(n,len));n+=len;}}
    else if(type==fu){size_t n=hevc?2:1;if(n+1>=data.size())return invalid();auto h=data[n++];if(h&128){annex();if(hevc){s.unit.push_back((data[0]&129)|((h&63)<<1));s.unit.push_back(data[1]);}else s.unit.push_back((data[0]&224)|(h&31));s.fragment=true;s.key|=hevc?((h&63)>=16&&(h&63)<=21):(h&31)==5;}
      else if(!s.fragment||gap)return invalid();s.unit.insert(s.unit.end(),data.begin()+n,data.end());if(h&64)s.fragment=false;
    }else if(type<(hevc?48:24)&&(hevc||type>0)){if(s.fragment)return invalid();nal(data);}else return invalid();
  }
  if(s.unit.size()>s.options.max_bytes)return invalid();
  if(!(p[1]&128)&&s.options.codec!="opus")return EncodedWriteStatus::accepted;
  if(s.fragment)return invalid();
  if(s.wait_key&&!s.key)return invalid();s.wait_key=false;
  if(!s.base){s.base=ts;s.epoch=std::chrono::steady_clock::now();}
  EncodedMediaFrame frame;frame.codec=s.options.codec;frame.width=s.options.width;frame.height=s.options.height;frame.keyframe=s.key;frame.rtp_timestamp=ts;frame.data=std::move(s.unit);
  const auto rate=s.options.codec=="opus"?48000:90000;frame.captured=s.epoch+std::chrono::microseconds(int64_t(int32_t(ts-*s.base))*1000000/rate);
  if(std::chrono::abs(std::chrono::steady_clock::now()-frame.captured)>std::chrono::seconds(2)){s.base=ts;s.epoch=frame.captured=std::chrono::steady_clock::now();}
  s.clear();return s.source->write(std::move(frame));
}
}
