#include "engines/rtc/rpc_session.hpp"
#include "engines/rtc/data_messages.hpp"
#include "livekit/error.hpp"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <iomanip>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

namespace livekit::sdk::detail {
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace {
constexpr size_t max_payload=15360,max_pending=128,max_packets=512;
using Key=std::pair<std::string,std::string>;
bool identifier(const std::string& s,size_t limit){return !s.empty()&&s.size()<=limit&&s.find('\0')==std::string::npos&&valid_utf8(s);}
std::string request_id(){std::random_device random;std::ostringstream text;for(int i=0;i<4;++i)text<<std::hex<<std::setw(8)<<std::setfill('0')<<random();return text.str();}
RpcError failure(RpcErrorCode code){
  const char* message="RPC application error";
  switch(code){
    case RpcErrorCode::unsupported_method:message="Method not supported at destination";break;
    case RpcErrorCode::unsupported_version:message="Unsupported RPC version";break;
    case RpcErrorCode::request_payload_too_large:message="Request payload too large";break;
    case RpcErrorCode::response_payload_too_large:message="Response payload too large";break;
    case RpcErrorCode::connection_timeout:message="RPC acknowledgement timeout";break;
    case RpcErrorCode::response_timeout:message="RPC response timeout";break;
    case RpcErrorCode::recipient_disconnected:message="RPC participant disconnected";break;
    case RpcErrorCode::send_failed:message="RPC send failed";break;
    default:break;
  }
  return {code,message};
}
std::string truncate_utf8(std::string value,size_t limit){if(value.size()>limit){value.resize(limit);while(!valid_utf8(value))value.pop_back();}return value;}
}
struct RpcSession::State {
  struct Outgoing {
    std::string destination;
    std::promise<std::string> promise;
    Clock::time_point deadline,ack_deadline;
    std::stop_token stop;
    bool acknowledged=false,sent=false;
  };
  struct Incoming {
    std::string method,payload;
    Clock::time_point deadline,expires;
    std::stop_source stop;
    uint64_t epoch=0,incarnation=0;
    bool completed=false;
    livekit::DataPacket response;
  };
  mutable std::mutex mutex;
  std::condition_variable_any changed;
  bool active=false,closed=false;
  uint64_t epoch=0,next_incarnation=0;
  std::map<std::string,RpcHandler> methods;
  std::map<std::string,std::shared_ptr<Outgoing>> outgoing;
  std::map<Key,Incoming> incoming;
  std::deque<livekit::DataPacket> packets;
  std::deque<std::function<void()>> handlers;

  bool enqueue(livekit::DataPacket packet){
    if(!active||closed||packets.size()>=max_packets)return false;
    packets.push_back(std::move(packet));return true;
  }
  void respond(const Key& key,Incoming& request,RpcResult result){
    if(request.completed||!active||request.epoch!=epoch)return;
    if(result.payload.size()>max_payload)result.error=failure(RpcErrorCode::response_payload_too_large);
    if(!valid_utf8(result.payload))result.error=failure(RpcErrorCode::application_error);
    livekit::DataPacket packet;packet.add_destination_identities(key.first);
    auto* response=packet.mutable_rpc_response();response->set_request_id(key.second);
    if(result.error){
      const auto& error=*result.error;std::string message=error.what(),data=error.data();
      if(!valid_utf8(message)||!valid_utf8(data)){message="Invalid RPC error text";data.clear();}
      auto* wire=response->mutable_error();wire->set_code(error.code());
      wire->set_message(truncate_utf8(std::move(message),256));wire->set_data(truncate_utf8(std::move(data),max_payload));
    }else response->set_payload(std::move(result.payload));
    request.completed=true;request.expires=Clock::now()+60s;request.response=packet;
    enqueue(std::move(packet));
  }
};
struct RpcSession::Threads {std::jthread timer,dispatcher;};
RpcSession::RpcSession():state_(std::make_shared<State>()),threads_(std::make_unique<Threads>()){
  auto state=state_;
  threads_->timer=std::jthread([state](std::stop_token stop){
    while(!stop.stop_requested()){
      std::vector<std::stop_source> cancelled;
      {
        std::unique_lock lock(state->mutex);
        state->changed.wait_for(lock,stop,10ms,[]{return false;});
        const auto now=Clock::now();
        for(auto it=state->outgoing.begin();it!=state->outgoing.end();){
          auto& request=*it->second;std::exception_ptr error;
          if(request.stop.stop_requested())error=std::make_exception_ptr(SDKError(ErrorDomain::lifecycle,
              request.sent?ErrorCode::outcome_unknown:ErrorCode::cancelled,"RPC cancelled; remote execution may already have occurred"));
          else if(now>=request.deadline)error=std::make_exception_ptr(failure(RpcErrorCode::response_timeout));
          else if(!request.acknowledged&&now>=request.ack_deadline)error=std::make_exception_ptr(failure(RpcErrorCode::connection_timeout));
          if(error){request.promise.set_exception(error);it=state->outgoing.erase(it);}else ++it;
        }
        for(auto it=state->incoming.begin();it!=state->incoming.end();){
          auto& request=it->second;
          if(!request.completed&&now>=request.deadline){cancelled.push_back(request.stop);state->respond(it->first,request,{{},failure(RpcErrorCode::response_timeout)});}
          if(request.completed&&now>=request.expires)it=state->incoming.erase(it);else ++it;
        }
      }
      for(auto& source:cancelled)source.request_stop();
    }
  });
  threads_->dispatcher=std::jthread([state](std::stop_token stop){
    while(!stop.stop_requested()){
      std::function<void()> handler;
      {std::unique_lock lock(state->mutex);state->changed.wait(lock,stop,[&]{return state->closed||!state->handlers.empty();});
       if(stop.stop_requested()||state->closed)break;handler=std::move(state->handlers.front());state->handlers.pop_front();}
      try{handler();}catch(...){/* Application/allocator failure must not terminate the native process. */}
    }
  });
}
RpcSession::~RpcSession(){
  disconnect();
  {std::lock_guard lock(state_->mutex);state_->closed=true;state_->handlers.clear();}
  threads_->timer.request_stop();threads_->dispatcher.request_stop();state_->changed.notify_all();
  threads_->timer.join();
  // Application async starters are required to return promptly, but must never
  // make Room destruction unbounded. The detached worker owns only State, not Room.
  threads_->dispatcher.detach();
}
void RpcSession::connect(){std::lock_guard lock(state_->mutex);if(!state_->active){++state_->epoch;state_->active=true;}}
void RpcSession::disconnect(){
  std::vector<std::stop_source> stops;
  {std::lock_guard lock(state_->mutex);state_->active=false;++state_->epoch;
   for(auto& [id,request]:state_->outgoing)request->promise.set_exception(std::make_exception_ptr(failure(RpcErrorCode::recipient_disconnected)));
   for(auto& [key,request]:state_->incoming)if(!request.completed)stops.push_back(request.stop);
   state_->outgoing.clear();state_->incoming.clear();state_->packets.clear();state_->handlers.clear();}
  for(auto& stop:stops)stop.request_stop();
}
void RpcSession::participant_left(const std::string& identity){
  std::vector<std::stop_source> stops;
  {std::lock_guard lock(state_->mutex);
   for(auto it=state_->outgoing.begin();it!=state_->outgoing.end();)if(it->second->destination==identity){
     it->second->promise.set_exception(std::make_exception_ptr(failure(RpcErrorCode::recipient_disconnected)));it=state_->outgoing.erase(it);
   }else ++it;
   for(auto it=state_->incoming.begin();it!=state_->incoming.end();)if(it->first.first==identity){stops.push_back(it->second.stop);it=state_->incoming.erase(it);}else ++it;
   std::erase_if(state_->packets,[&](const auto& packet){return packet.destination_identities_size()==1&&packet.destination_identities(0)==identity;});}
  for(auto& stop:stops)stop.request_stop();
}
void RpcSession::register_method(const std::string& method,RpcHandler handler){
  if(!identifier(method,256)||!handler)throw std::invalid_argument("RPC method and handler required");
  std::lock_guard lock(state_->mutex);
  if(state_->methods.contains(method))throw std::invalid_argument("RPC method already registered");
  if(state_->methods.size()>=64)throw SDKError(ErrorDomain::lifecycle,ErrorCode::resource_exhausted,"RPC method limit");
  state_->methods.emplace(method,std::move(handler));
}
void RpcSession::unregister_method(const std::string& method){std::lock_guard lock(state_->mutex);state_->methods.erase(method);}
std::future<std::string> RpcSession::call(const RpcCallOptions& options){
  auto request=std::make_shared<State::Outgoing>();auto result=request->promise.get_future();
  try{
    if(!identifier(options.destination_identity,1024)||!identifier(options.method,256)||!valid_utf8(options.payload))throw std::invalid_argument("Invalid RPC identity, method or UTF-8 payload");
    if(options.payload.size()>max_payload)throw failure(RpcErrorCode::request_payload_too_large);
    if(options.response_timeout<=0ms||options.response_timeout>5min)throw std::invalid_argument("RPC timeout must be positive and at most five minutes");
    if(options.stop.stop_requested())throw SDKError(ErrorDomain::lifecycle,ErrorCode::cancelled,"RPC cancelled before sending");
    const auto now=Clock::now();request->destination=options.destination_identity;request->deadline=now+options.response_timeout;
    request->ack_deadline=now+7s;request->stop=options.stop;
    const auto id=request_id();livekit::DataPacket packet;packet.add_destination_identities(options.destination_identity);
    auto* wire=packet.mutable_rpc_request();wire->set_id(id);wire->set_method(options.method);wire->set_payload(options.payload);wire->set_version(1);
    wire->set_response_timeout_ms(static_cast<uint32_t>(std::max(1000ms,options.response_timeout-7000ms).count()));
    std::lock_guard lock(state_->mutex);
    if(!state_->active)throw failure(RpcErrorCode::recipient_disconnected);
    if(state_->outgoing.size()>=max_pending||state_->packets.size()>=max_packets)throw SDKError(ErrorDomain::lifecycle,ErrorCode::resource_exhausted,"RPC pending limit");
    if(!state_->outgoing.emplace(id,request).second)throw std::runtime_error("RPC request ID collision");
    try{state_->packets.push_back(std::move(packet));}catch(...){state_->outgoing.erase(id);throw;}
  }catch(...){request->promise.set_exception(std::current_exception());}
  return result;
}
void RpcSession::receive(const livekit::DataPacket& packet){
  if(packet.kind()!=0||!identifier(packet.participant_identity(),1024)||packet.ByteSizeLong()>16384)return;
  const auto& sender=packet.participant_identity();std::lock_guard lock(state_->mutex);
  if(!state_->active)return;
  if(packet.has_rpc_ack()||packet.has_rpc_response()){
    const auto& id=packet.has_rpc_ack()?packet.rpc_ack().request_id():packet.rpc_response().request_id();
    auto found=state_->outgoing.find(id);if(found==state_->outgoing.end()||found->second->destination!=sender)return;
    auto& request=*found->second;
    const auto now=Clock::now();
    std::exception_ptr terminal;
    if(request.stop.stop_requested())terminal=std::make_exception_ptr(SDKError(ErrorDomain::lifecycle,
        request.sent?ErrorCode::outcome_unknown:ErrorCode::cancelled,"RPC cancelled; remote execution may already have occurred"));
    else if(now>=request.deadline)terminal=std::make_exception_ptr(failure(RpcErrorCode::response_timeout));
    else if(!request.acknowledged&&now>=request.ack_deadline)terminal=std::make_exception_ptr(failure(RpcErrorCode::connection_timeout));
    if(terminal){request.promise.set_exception(terminal);state_->outgoing.erase(found);return;}
    if(packet.has_rpc_ack()){request.acknowledged=true;return;}
    const auto& response=packet.rpc_response();
    if(response.has_error()){
      const auto& error=response.error();
      if(error.message().size()>256||error.data().size()>max_payload||!valid_utf8(error.message())||!valid_utf8(error.data()))return;
      request.promise.set_exception(std::make_exception_ptr(RpcError(error.code(),error.message(),error.data())));
    }else if(response.has_compressed_payload())request.promise.set_exception(std::make_exception_ptr(failure(RpcErrorCode::unsupported_version)));
    else if(response.has_payload()&&response.payload().size()<=max_payload&&valid_utf8(response.payload()))request.promise.set_value(response.payload());
    else request.promise.set_exception(std::make_exception_ptr(failure(RpcErrorCode::response_payload_too_large)));
    state_->outgoing.erase(found);return;
  }
  if(!packet.has_rpc_request())return;
  const auto& wire=packet.rpc_request();if(!identifier(wire.id(),128))return;
  const Key key{sender,wire.id()};
  livekit::DataPacket ack;ack.add_destination_identities(sender);ack.mutable_rpc_ack()->set_request_id(wire.id());
  if(!state_->enqueue(std::move(ack)))return;
  if(auto found=state_->incoming.find(key);found!=state_->incoming.end()){
    if(found->second.method!=wire.method()||found->second.payload!=wire.payload())return;
    if(found->second.completed)state_->enqueue(found->second.response);
    return;
  }
  if(state_->incoming.size()>=max_pending){
    // Evict only completed calls. In-flight requests are never replaced.
    auto oldest=state_->incoming.end();
    for(auto it=state_->incoming.begin();it!=state_->incoming.end();++it)if(it->second.completed&&(oldest==state_->incoming.end()||it->second.expires<oldest->second.expires))oldest=it;
    if(oldest!=state_->incoming.end())state_->incoming.erase(oldest);
    else {
      livekit::DataPacket busy;busy.add_destination_identities(sender);
      auto* response=busy.mutable_rpc_response();response->set_request_id(wire.id());
      response->mutable_error()->set_code(static_cast<uint32_t>(RpcErrorCode::application_error));
      response->mutable_error()->set_message("RPC receiver busy");state_->enqueue(std::move(busy));return;
    }
  }
  auto& request=state_->incoming[key];request.method=wire.method();request.payload=wire.payload();request.epoch=state_->epoch;
  request.incarnation=++state_->next_incarnation;
  request.deadline=Clock::now()+std::chrono::milliseconds(std::clamp(wire.response_timeout_ms(),1u,300000u));
  auto reject=[&](RpcErrorCode error){state_->respond(key,request,{{},failure(error)});};
  if(wire.version()!=1||!wire.compressed_payload().empty()){reject(RpcErrorCode::unsupported_version);return;}
  if(wire.payload().size()>max_payload){reject(RpcErrorCode::request_payload_too_large);return;}
  if(!identifier(wire.method(),256)||!valid_utf8(wire.payload())){reject(RpcErrorCode::application_error);return;}
  auto method=state_->methods.find(wire.method());if(method==state_->methods.end()){reject(RpcErrorCode::unsupported_method);return;}
  if(state_->handlers.size()>=32){reject(RpcErrorCode::application_error);return;}
  RpcInvocation invocation{wire.id(),sender,wire.payload(),request.deadline,request.stop.get_token()};
  const auto epoch=state_->epoch;
  const auto incarnation=request.incarnation;
  RpcCompletion done=[weak=std::weak_ptr<State>(state_),key,epoch,incarnation](RpcResult result){
    if(auto state=weak.lock()){std::lock_guard lock(state->mutex);auto found=state->incoming.find(key);
      if(found!=state->incoming.end()&&found->second.epoch==epoch&&found->second.incarnation==incarnation&&Clock::now()<found->second.deadline)state->respond(key,found->second,std::move(result));}
  };
  state_->handlers.push_back([handler=method->second,invocation=std::move(invocation),done=std::move(done)]()mutable{
    if(invocation.stop.stop_requested()||Clock::now()>=invocation.deadline)return;
    try{handler(std::move(invocation),done);}catch(const RpcError& error){done({{},error});}catch(...){done({{},failure(RpcErrorCode::application_error)});}
  });
  state_->changed.notify_all();
}
bool RpcSession::has_outgoing() const{std::lock_guard lock(state_->mutex);return !state_->packets.empty();}
std::vector<livekit::DataPacket> RpcSession::take_packets(size_t count){
  std::vector<livekit::DataPacket> result;std::lock_guard lock(state_->mutex);
  while(!state_->packets.empty()&&result.size()<count){
    auto packet=std::move(state_->packets.front());state_->packets.pop_front();
    if(packet.has_rpc_request()){
      auto found=state_->outgoing.find(packet.rpc_request().id());
      if(found==state_->outgoing.end()||found->second->stop.stop_requested()||Clock::now()>=found->second->deadline)continue;
      found->second->sent=true;
    }
    result.push_back(std::move(packet));
  }
  return result;
}
void RpcSession::send_failed(const livekit::DataPacket& packet){
  if(!packet.has_rpc_request())return;std::lock_guard lock(state_->mutex);
  auto found=state_->outgoing.find(packet.rpc_request().id());if(found==state_->outgoing.end())return;
  found->second->promise.set_exception(std::make_exception_ptr(failure(RpcErrorCode::send_failed)));state_->outgoing.erase(found);
}
}
