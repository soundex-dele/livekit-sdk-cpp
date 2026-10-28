#pragma once
#include "engines/rtc/rpc_session.hpp"
#include "livekit/error.hpp"
#include "support/compression.hpp"
#include <atomic>
#include <source_location>
#include <thread>

namespace rpc_contract_test {
using namespace livekit::sdk;
using namespace std::chrono_literals;
inline void check(bool ok,const std::source_location at=std::source_location::current()) {
  if(!ok)throw std::runtime_error("RPC contract failed at line "+std::to_string(at.line()));
}
template<class T> void ready(std::future<T>& future){check(future.wait_for(2s)==std::future_status::ready);}
inline void rpc_error(std::future<std::string>& future,RpcErrorCode code){
  ready(future);bool caught=false;try{future.get();}catch(const RpcError& e){caught=e.code()==static_cast<uint32_t>(code);}check(caught);
}
inline void sdk_error(std::future<std::string>& future,ErrorCode code){
  ready(future);bool caught=false;try{future.get();}catch(const SDKError& e){caught=e.code()==code;}check(caught);
}
inline livekit::DataPacket request(std::string id,std::string method="echo",std::string payload="hello 世界"){
  livekit::DataPacket packet;packet.set_participant_identity("peer");
  auto* wire=packet.mutable_rpc_request();wire->set_id(std::move(id));wire->set_method(std::move(method));
  wire->set_payload(std::move(payload));wire->set_version(1);wire->set_response_timeout_ms(1000);return packet;
}
inline livekit::DataPacket response(const livekit::DataPacket& request,std::string sender="peer"){
  livekit::DataPacket packet;packet.set_participant_identity(sender);
  packet.mutable_rpc_response()->set_request_id(request.rpc_request().id());packet.mutable_rpc_response()->set_payload("reply");return packet;
}
inline livekit::RpcResponse read_response(detail::RpcSession& session,const std::string& id){
  const auto end=std::chrono::steady_clock::now()+2s;
  do {for(auto& packet:session.take_packets(512))if(packet.has_rpc_response()&&packet.rpc_response().request_id()==id)return packet.rpc_response();
    std::this_thread::sleep_for(1ms);
  }while(std::chrono::steady_clock::now()<end);
  throw std::runtime_error("RPC response deadline: "+id);
}
inline void run(){
  detail::RpcSession session;session.connect();
  RpcCallOptions call{"peer","echo","hello"};
  // Response may precede ACK; spoofed identities and lossy packets cannot settle calls.
  auto result=session.call(call);auto packets=session.take_packets();check(packets.size()==1);
  session.receive(response(packets[0],"attacker"));check(result.wait_for(0ms)!=std::future_status::ready);
  auto reply=response(packets[0]);reply.set_kind(1);session.receive(reply);
  check(result.wait_for(0ms)!=std::future_status::ready);reply.set_kind(0);
  session.receive(reply);ready(result);check(result.get()=="reply");session.receive(reply);
  result=session.call(call);packets=session.take_packets();reply=response(packets[0]);
  reply.mutable_rpc_response()->set_compressed_payload("unsupported");session.receive(reply);rpc_error(result,RpcErrorCode::response_payload_too_large);
  result=session.call(call);packets=session.take_packets();reply=response(packets[0]);reply.mutable_rpc_response()->set_compressed_payload(detail::deflate_raw("compressed reply"));session.receive(reply);ready(result);check(result.get()=="compressed reply");
  // Error payload is retained exactly.
  result=session.call(call);packets=session.take_packets();reply=response(packets[0]);
  reply.mutable_rpc_response()->mutable_error()->set_code(2010);reply.mutable_rpc_response()->mutable_error()->set_message("拒绝");
  reply.mutable_rpc_response()->mutable_error()->set_data("reason");session.receive(reply);ready(result);
  try{result.get();check(false);}catch(const RpcError& e){check(e.code()==2010&&e.data()=="reason"&&std::string(e.what())=="拒绝");}
  // Cancelled work must never be taken, and responses cannot win after stop was requested.
  std::stop_source before;call.stop=before.get_token();before.request_stop();result=session.call(call);sdk_error(result,ErrorCode::cancelled);check(session.take_packets().empty());
  std::stop_source queued;call.stop=queued.get_token();result=session.call(call);queued.request_stop();check(session.take_packets().empty());sdk_error(result,ErrorCode::cancelled);
  std::stop_source sent;call.stop=sent.get_token();result=session.call(call);packets=session.take_packets();sent.request_stop();session.receive(response(packets[0]));sdk_error(result,ErrorCode::outcome_unknown);call.stop={};
  call.response_timeout=25ms;result=session.call(call);rpc_error(result,RpcErrorCode::response_timeout);check(session.take_packets().empty());
  call.response_timeout=15s;result=session.call(call);packets=session.take_packets();session.send_failed(packets[0]);rpc_error(result,RpcErrorCode::send_failed);
  call.payload=std::string(15361,'x');result=session.call(call);rpc_error(result,RpcErrorCode::request_payload_too_large);call.payload="hello";
  result=session.call(call);session.participant_left("peer");rpc_error(result,RpcErrorCode::recipient_disconnected);check(session.take_packets().empty());
  result=session.call(call);session.disconnect();rpc_error(result,RpcErrorCode::recipient_disconnected);session.connect();
  // Request/ACK/response round trip, duplicate requests and duplicate completion.
  std::atomic<int> handled=0;
  session.register_method("echo",[&](RpcInvocation invocation,RpcCompletion done){
    check(invocation.caller_identity=="peer");++handled;done({invocation.payload,{}});done({"ignored",{}});
  });
  auto req=request("one");session.receive(req);check(read_response(session,"one").payload()=="hello 世界");
  session.receive(req);check(read_response(session,"one").payload()=="hello 世界"&&handled==1);
  session.receive(request("unknown","absent"));check(read_response(session,"unknown").error().code()==1400);
  req=request("version");req.mutable_rpc_request()->set_version(2);session.receive(req);check(read_response(session,"version").error().code()==1404);
  req=request("compressed");req.mutable_rpc_request()->set_compressed_payload("x");session.receive(req);check(read_response(session,"compressed").error().code()==1402);
  req=request("compressed-valid");req.mutable_rpc_request()->clear_payload();req.mutable_rpc_request()->set_compressed_payload(detail::deflate_raw("hello 世界"));session.receive(req);check(read_response(session,"compressed-valid").payload()=="hello 世界");
  session.receive(request("large","echo",std::string(15361,'x')));check(read_response(session,"large").error().code()==1402);
  session.register_method("throw",[](auto,auto){throw std::runtime_error("private application secret");});
  session.receive(request("exception","throw"));auto error=read_response(session,"exception").error();check(error.code()==1500&&error.message().find("secret")==std::string::npos);
  session.register_method("huge",[](auto,RpcCompletion done){done({std::string(15361,'x'),{}});});
  session.receive(request("huge","huge"));check(read_response(session,"huge").error().code()==1504);
  session.unregister_method("echo");session.receive(request("removed"));check(read_response(session,"removed").error().code()==1400);
  // An old completion must not satisfy a new invocation with the same key/transport epoch.
  std::promise<std::pair<RpcInvocation,RpcCompletion>> first,second;
  auto f1=first.get_future(),f2=second.get_future();int generation=0;
  session.register_method("hold",[&](auto invocation,auto done){(generation++==0?first:second).set_value({invocation,done});});
  session.receive(request("reused","hold"));ready(f1);auto old=f1.get();session.take_packets();
  session.participant_left("peer");check(old.first.stop.stop_requested());
  session.receive(request("reused","hold"));ready(f2);auto current=f2.get();session.take_packets();old.second({"stale",{}});check(session.take_packets().empty());
  current.second({"current",{}});check(read_response(session,"reused").payload()=="current");
  // Deadline cancellation works independently of application completion.
  std::promise<RpcInvocation> incoming;auto invocation=incoming.get_future();
  session.register_method("timeout",[&](auto info,auto){incoming.set_value(info);});
  req=request("timeout","timeout");req.mutable_rpc_request()->set_response_timeout_ms(200);session.receive(req);ready(invocation);
  auto timed=invocation.get();check(read_response(session,"timeout").error().code()==1502);
  for(int i=0;i<100&&!timed.stop.stop_requested();++i)std::this_thread::sleep_for(1ms);check(timed.stop.stop_requested());
  // Bounded outbound work and ACK timeout (one real seven-second wait).
  std::vector<std::future<std::string>> pending;
  for(int i=0;i<128;++i)pending.push_back(session.call(call));result=session.call(call);sdk_error(result,ErrorCode::resource_exhausted);
  session.disconnect();for(auto& future:pending)rpc_error(future,RpcErrorCode::recipient_disconnected);session.connect();
  result=session.call(call);session.take_packets();check(result.wait_for(8s)==std::future_status::ready);rpc_error(result,RpcErrorCode::connection_timeout);
  // Full inbound cache rejects explicitly after ACK; cancellation callbacks may reenter.
  std::promise<void> cancel_observed;auto cancellation=cancel_observed.get_future();
  using Callback=std::stop_callback<std::function<void()>>;
  std::vector<std::unique_ptr<Callback>> cancellation_callbacks;
  session.register_method("capacity",[&](RpcInvocation invocation,RpcCompletion){
    if(cancellation_callbacks.empty())cancellation_callbacks.push_back(std::make_unique<Callback>(invocation.stop,std::function<void()>([&]{session.unregister_method("throw");cancel_observed.set_value();})));
  });
  session.disconnect();session.connect();
  // Wait for each starter to run to avoid testing the separate dispatch queue bound here.
  std::atomic<int> started=0;
  session.register_method("count",[&](auto,auto){++started;});
  for(int i=0;i<128;++i){
    auto pending=request("capacity-"+std::to_string(i),i?"count":"capacity");pending.mutable_rpc_request()->set_response_timeout_ms(300000);
    session.receive(pending);session.take_packets();
    if(i)for(int spins=0;started<i&&spins<2000;++spins)std::this_thread::sleep_for(1ms);
  }
  check(started==127);session.receive(request("overflow"));check(read_response(session,"overflow").error().code()==1500);
  session.disconnect();ready(cancellation);cancellation.get();cancellation_callbacks.clear();session.connect();
  // Completed-entry eviction cannot authorize a stale completion for a reused key.
  std::promise<RpcCompletion> cached_first,cached_second;auto c1=cached_first.get_future(),c2=cached_second.get_future();
  std::atomic<int> cache_generation=0;
  session.register_method("cache",[&](auto,auto done){(cache_generation++==0?cached_first:cached_second).set_value(done);});
  session.receive(request("evicted","cache"));ready(c1);auto stale=c1.get();stale({"first",{}});read_response(session,"evicted");
  // Ensure first completion has the oldest expiry even on a coarse clock.
  std::this_thread::sleep_for(2ms);
  for(int i=0;i<128;++i){const auto id="complete-"+std::to_string(i);session.receive(request(id,"absent"));read_response(session,id);}
  session.receive(request("evicted","cache"));ready(c2);auto fresh=c2.get();session.take_packets();
  stale({"wrong",{}});check(session.take_packets().empty());fresh({"second",{}});check(read_response(session,"evicted").payload()=="second");
  session.disconnect(); // Finish cancellation before captured test state is destroyed.
}
}
inline void rpc_contract(){rpc_contract_test::run();}
