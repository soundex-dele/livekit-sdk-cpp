#pragma once
#include "audio_source_integration.hpp"
#include <filesystem>
namespace go_interop_test {
inline int run_rpc_only(const char* url,const char* directory) {
  using namespace audio_source_test;
  const std::filesystem::path out(directory);
  Room room;
  room.register_rpc_method("cpp.echo",[](RpcInvocation invocation,RpcCompletion done){done({"C++:"+invocation.payload,{}});});
  room.register_rpc_method("cpp.error",[](RpcInvocation,RpcCompletion done){done({{},RpcError(2001,"拒绝","cpp-details")});});
  const auto token=AccessToken("reconnect-test","reconnect-integration-secret-32-characters").set_identity("cpp-peer")
      .set_video_grant({{"roomJoin",true},{"room","go-interop"},{"canPublish",true},{"canSubscribe",true},{"canPublishData",true}}).to_jwt();
  room.join_with_token(url,token);
  check(room.local_participant().info().tracks.empty(),"RPC-only peer published media");
  auto rpc=room.local_participant().perform_rpc({"go-peer","go.echo","你好 🌍"});
  check(rpc.wait_for(16s)==std::future_status::ready&&rpc.get()=="Go:你好 🌍","RPC-only publisher negotiation failed");
  rpc=room.local_participant().perform_rpc({"go-peer","go.error",""});bool caught=false;
  try{rpc.get();}catch(const RpcError& e){caught=e.code()==2002&&e.data()=="go-details";}check(caught,"RPC-only error failed");
  rpc=room.local_participant().perform_rpc({"go-peer","missing",""});caught=false;
  try{rpc.get();}catch(const RpcError& e){caught=e.code()==1400;}check(caught,"RPC-only missing method failed");
  std::ofstream(out/"cpp-result.json")<<Json{{"ok",true},{"no_media",true},{"rpc_echo_error_unknown",true}}.dump();
  until([&]{return std::filesystem::exists(out/"go-result.json");},"Go RPC-only result missing");
  std::ofstream(out/"stop")<<"done";room.disconnect();room.wait();return 0;
}
inline int run(const char* url, const char* directory) {
  using namespace audio_source_test;
  const std::filesystem::path out(directory);
  auto source = std::make_shared<PcmAudioSource>(PcmSourceOptions{16000,1,AudioSampleFormat::pcm16});
  auto stream = std::make_shared<AudioFrameStream>(AudioStreamOptions{500,4*1024*1024});
  std::atomic<bool> dtmf{false},partial{false},final{false};
  RoomCallback callbacks;
  callbacks.on_dtmf_received=[&](const DtmfDigit& d,const DataReceiveParams& p){std::cerr<<"DTMF sender="<<p.sender_identity<<" code="<<d.code<<'\n';if(p.sender_identity=="go-peer" && p.reliable && d.code==10 && d.digit=="*")dtmf=true;};
  callbacks.on_transcription_received=[&](const Transcription& t,const DataReceiveParams& p){
    std::cerr<<"Transcription sender="<<p.sender_identity<<" subject="<<t.participant_identity<<" segments="<<t.segments.size()<<'\n';
    if(p.sender_identity!="go-peer" || t.participant_identity!="go-peer" || t.track_id.empty())return;
    for(const auto& s:t.segments)if(s.id=="go-segment" && s.start_time==123 && s.end_time==456 && s.language=="en") {
      if(s.final && s.text=="hello 世界")final=true; else if(!s.final && s.text=="hello")partial=true;
    }
  };
  Room room(callbacks); RoomOptions options; options.audio_frame_stream = stream;
  room.register_rpc_method("cpp.echo",[](RpcInvocation invocation,RpcCompletion done){
    if(invocation.caller_identity!="go-peer")throw std::runtime_error("Unexpected caller");
    done({"C++:"+invocation.payload,{}});
  });
  room.register_rpc_method("cpp.error",[](RpcInvocation,RpcCompletion done){done({{},RpcError(2001,"拒绝","cpp-details")});});
  auto token=AccessToken("reconnect-test","reconnect-integration-secret-32-characters")
      .set_identity("cpp-peer").set_kind("agent")
      .set_video_grant({{"roomJoin",true},{"room","go-interop"},{"canPublish",true},{"canSubscribe",true},{"canPublishData",true}}).to_jwt();
  room.join_with_token(url,token,options);
  const auto capabilities=room.capabilities();
  check(capabilities.connected&&capabilities.signal_protocol==12&&capabilities.single_peer_connection==false&&
      !capabilities.server.version.empty()&&capabilities.server.advertised_protocol.has_value(),"Joined capabilities missing");
  check(capabilities.server.support(ClientFeature::rpc)==CapabilitySupport::unknown,"Server RPC support was guessed");
  MediaSpec media; media.video = false; media.audio_source = source;
  TrackPublishOptions publication; publication.name = "cpp-440"; publication.disable_red = true;
  const auto published=room.local_participant().publish_track(media, publication).get();
  std::jthread writer([&](std::stop_token stop) {
    uint64_t index=0;
    while (!stop.stop_requested()) {
      PcmInputFrame pcm; pcm.samples.resize(320);
      for (size_t i=0;i<pcm.samples.size();++i) pcm.samples[i]=static_cast<int16_t>(6000*std::sin(2*std::numbers::pi*440*(index+i)/16000));
      if (source->write(pcm)==AudioWriteStatus::accepted) index+=320;
      std::this_thread::sleep_for(2ms);
    }
  });
  Tone tone; bool audible=false; const auto deadline=std::chrono::steady_clock::now()+35s;
  while (tone.count<96000) {
    check(std::chrono::steady_clock::now()<deadline,"Go PCM receive deadline exceeded");
    const auto result=stream->read(1s);
    if (result.status!=AudioReadStatus::frame) continue;
    if (!audible) {
      audible=std::any_of(result.frame->samples.begin(),result.frame->samples.end(),[](auto v){return std::abs(int(v))>500;});
      if (!audible) continue;
    }
    tone.add(*result.frame,0);
  }
  const auto frequency=tone.frequency();
  std::cerr<<"Go to C++ frequency="<<frequency<<" samples="<<tone.count<<'\n';
  check(std::abs(frequency-880)<44 && tone.peak>500,"Go to C++ audio frequency mismatch");
  room.local_participant().publish_dtmf({11,"#"}).get();
  Transcription transcript{"cpp-peer",published.sid(),{{"cpp-segment","你好",123,456,false,"zh"}}};
  room.local_participant().publish_transcription(transcript).get();
  transcript.segments[0].text="你好 🌍"; transcript.segments[0].final=true;
  room.local_participant().publish_transcription(transcript).get();
  until([&]{return dtmf.load() && partial.load() && final.load();},"Go official data messages missing");
  auto rpc=room.local_participant().perform_rpc({"go-peer","go.echo","你好 🌍"});
  check(rpc.wait_for(16s)==std::future_status::ready&&rpc.get()=="Go:你好 🌍","C++ to Go RPC echo failed");
  rpc=room.local_participant().perform_rpc({"go-peer","go.error",""});
  bool rpc_error=false;
  try{rpc.get();}catch(const RpcError& error){rpc_error=error.code()==2002&&error.data()=="go-details";}
  check(rpc_error,"Go RPC error did not round trip");
  rpc=room.local_participant().perform_rpc({"go-peer","missing",""});rpc_error=false;
  try{rpc.get();}catch(const RpcError& error){rpc_error=error.code()==1400;}
  check(rpc_error,"Go unknown RPC method code mismatch");
  std::ofstream(out/"cpp-result.json") << Json{{"ok",true},{"received_hz",frequency},{"samples",tone.count},{"peak",tone.peak},{"dtmf",true},{"partial_final_transcription",true},{"rpc_echo_error_unknown",true}}.dump();
  until([&]{return std::filesystem::exists(out/"go-result.json");},"Go failed to decode C++ PCM");
  std::ofstream(out/"stop") << "done";
  writer.request_stop(); writer.join(); source->close(); room.disconnect(); room.wait();
  check(!room.capabilities().connected&&!room.capabilities().signal_protocol,"Disconnected capabilities stale");
  return 0;
}
}
