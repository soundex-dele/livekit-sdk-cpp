#include <livekit/remote_control.hpp>
#include "control_input_win.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace livekit::sdk;
using namespace std::chrono_literals;
struct Recorder:ControlInputSink {std::vector<ControlInput> events;bool fail=false;bool apply(const ControlInput& e)noexcept override{events.push_back(e);return !fail;}};
void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
int main(int argc,char**argv){try{
  auto sink=std::make_shared<Recorder>();RemoteControlSession session(sink,false);auto now=ControlClock::now();
  ControlGrant grant{"grant","controller",1,now+10s};session.authorize(grant,{1,-100,20,1000,500},now);
  DataReceiveParams params{"controller",remote_control_topic,true};uint64_t seq=0;
  auto send=[&](Json v){v["v"]=1;v["grant_id"]="grant";v["epoch"]=1;v["seq"]=++seq;v["layout_version"]=1;auto text=v.dump();return session.receive({text.begin(),text.end()},params,now);};
  check(!send({{"type","key"},{"code","KeyA"},{"down",true}}),"requires layout acknowledgement");
  check(send({{"type","layout_ack"}}),"ack");
  params.sender_identity="intruder";check(!send({{"type","key"},{"code","KeyA"},{"down",true}}),"identity");params.sender_identity="controller";
  check(send({{"type","key"},{"code","KeyA"},{"down",true}}),"key down");
  check(!send({{"type","key"},{"code","FakeKey"},{"down",true}}),"unknown key");
  check(send({{"type","button"},{"x",1.0},{"y",0.0},{"button",0},{"down",true}}),"button");
  check(sink->events.back().x==899&&sink->events.back().y==20,"coordinate mapping");
  check(!send({{"type","move"},{"x",1.01},{"y",0.0}}),"bounds");
  check(!send({{"type","button"},{"x",0.5},{"y",0.5},{"button",4294967296ULL},{"down",true}}),"button integer overflow");
  check(!send({{"type","wheel"},{"x",0.5},{"y",0.5},{"delta",1.5}}),"fractional wheel");
  session.tick(now+3s);check(!session.active(),"heartbeat timeout");check(sink->events.size()==4&&!sink->events[2].down&&!sink->events[3].down,"release pressed keys and buttons");
  check(!session.renew(grant,now+4s),"cannot resurrect");
  session.authorize(grant,{1,0,0,100,100},now);seq=0;check(send({{"type","layout_ack"}}),"ack again");
  auto text=Json{{"v",1},{"grant_id","grant"},{"epoch",1},{"seq",1},{"type","heartbeat"}}.dump();check(!session.receive({text.begin(),text.end()},params,now),"replay");
  check(send({{"type","key"},{"code","ShiftLeft"},{"down",true}}),"shift");session.set_layout({2,0,0,200,200});check(!sink->events.back().down,"layout releases");check(!send({{"type","move"},{"x",0.5},{"y",0.5}}),"stale layout");
  session.tick(now+10s);check(session.reason()=="lease_expired","expiry");
  session.authorize(grant,{1,0,0,100,100},now);seq=0;check(send({{"type","layout_ack"}}),"ack");
  sink->fail=true;check(!send({{"type","key"},{"code","KeyB"},{"down",true}}),"injection failure stops control");check(!session.active(),"failed input inactive");sink->fail=false;session.tick(now);check(sink->events.back().code=="KeyB"&&!sink->events.back().down,"failed release retried");
  if(argc>1){std::ifstream file(argv[1]);Json fixture;file>>fixture;RemoteControlSession contract(sink,false);contract.authorize({"example","controller_example",1,now+10s},{1,0,0,1920,1080},now);for(auto&message:fixture.at("controller_messages")){auto raw=message.dump();check(contract.receive({raw.begin(),raw.end()},{"controller_example",remote_control_topic,true},now),"shared protocol fixture");}}
#ifdef _WIN32
  struct FakeApi:detail::WindowsInputApi {bool available=true;std::vector<INPUT> inputs;bool desktop_available()noexcept override{return available;}ControlLayout virtual_layout()noexcept override{return {1,-1920,0,3840,1080};}unsigned send(std::span<const INPUT> value)noexcept override{inputs.assign(value.begin(),value.end());return static_cast<unsigned>(value.size());}};
  auto api=std::make_shared<FakeApi>();auto native=detail::windows_input_with_api(api);ControlInput input;input.type="key";input.code="ControlRight";input.down=true;check(native->apply(input),"native key");check(api->inputs[0].ki.wScan==0x1d&&(api->inputs[0].ki.dwFlags&KEYEVENTF_EXTENDEDKEY),"extended scan code");input.down=false;native->apply(input);check(api->inputs[0].ki.dwFlags&KEYEVENTF_KEYUP,"native keyup");
  input={};input.type="button";input.positioned=true;input.button=2;input.down=true;input.x=1919;input.y=1079;check(native->apply(input),"native button");check(api->inputs.size()==2&&api->inputs[0].mi.dx==65535&&api->inputs[0].mi.dy==65535&&api->inputs[1].mi.dwFlags==MOUSEEVENTF_RIGHTDOWN,"virtual desktop normalization");
  input.positioned=false;input.down=false;native->apply(input);check(api->inputs.size()==1&&api->inputs[0].mi.dwFlags==MOUSEEVENTF_RIGHTUP,"release does not move pointer");api->available=false;check(!native->apply(input),"secure desktop denied");
#endif
  std::cout<<"remote control tests passed\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
