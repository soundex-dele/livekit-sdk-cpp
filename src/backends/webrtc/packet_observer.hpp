#pragma once
#include "livekit/rtp.hpp"
#include "api/rtc_event_log/rtc_event_log_factory_interface.h"
#include "logging/rtc_event_log/events/rtc_event_rtp_packet_incoming.h"
#include "logging/rtc_event_log/events/rtc_event_rtp_packet_outgoing.h"
#include "logging/rtc_event_log/events/rtc_event_rtcp_packet_incoming.h"
#include "logging/rtc_event_log/events/rtc_event_rtcp_packet_outgoing.h"

namespace livekit::sdk::detail {
class PacketEventLog final:public webrtc::RtcEventLog {
 public:
  PacketEventLog(std::shared_ptr<PacketStream> output,uint64_t generation):output_(std::move(output)),generation_(generation){}
  bool StartLogging(std::unique_ptr<webrtc::RtcEventLogOutput>,int64_t)override{return false;}
  void StopLogging()override{}
  void Log(std::unique_ptr<webrtc::RtcEvent> event)override{
    try{switch(event->GetType()){
      case webrtc::RtcEvent::Type::RtpPacketIncoming:rtp(static_cast<const webrtc::RtcEventRtpPacketIncoming&>(*event),true);break;
      case webrtc::RtcEvent::Type::RtpPacketOutgoing:rtp(static_cast<const webrtc::RtcEventRtpPacketOutgoing&>(*event),false);break;
      case webrtc::RtcEvent::Type::RtcpPacketIncoming:rtcp(static_cast<const webrtc::RtcEventRtcpPacketIncoming&>(*event).packet(),true);break;
      case webrtc::RtcEvent::Type::RtcpPacketOutgoing:rtcp(static_cast<const webrtc::RtcEventRtcpPacketOutgoing&>(*event).packet(),false);break;
      default:break;
    }}catch(...){/* Observer allocation failure must not unwind native transport. */}
  }
 private:
  template<class T>void rtp(const T& packet,bool incoming){auto header=packet.RawHeader();emit(header.data(),header.size(),packet.packet_length(),incoming,false);}
  void rtcp(const webrtc::Buffer& packet,bool incoming){emit(packet.data(),packet.size(),packet.size(),incoming,true);}
  void emit(const uint8_t* data,size_t size,size_t total,bool incoming,bool rtcp){if(size>65535)return;PacketEvent event;event.bytes.assign(data,data+size);event.packet_size=total;event.incoming=incoming;event.rtcp=rtcp;event.generation=generation_;event.observed=std::chrono::steady_clock::now();output_->push(std::move(event));}
  std::shared_ptr<PacketStream> output_;uint64_t generation_;
};
class PacketEventLogFactory final:public webrtc::RtcEventLogFactoryInterface {
 public:
  PacketEventLogFactory(std::shared_ptr<PacketStream> output,uint64_t generation):output_(std::move(output)),generation_(generation){}
  std::unique_ptr<webrtc::RtcEventLog> Create(const webrtc::Environment&)const override{return std::make_unique<PacketEventLog>(output_,generation_);}
 private:std::shared_ptr<PacketStream> output_;uint64_t generation_;
};
}
