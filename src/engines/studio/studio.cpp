#include "internal.hpp"
#include "studio_graph.hpp"
#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#endif
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <set>
#include <thread>

namespace livekit::sdk {
using namespace studio_detail;
using Json=nlohmann::json;
namespace {
void validate(const StudioScene&s){if(s.sources.size()>32)throw std::invalid_argument("最多支持 32 个素材");std::set<std::string>ids;for(const auto&v:s.sources){if(v.id.empty()||!ids.insert(v.id).second)throw std::invalid_argument("素材 ID 无效或重复");auto&t=v.transform;for(float f:{t.x,t.y,t.width,t.height,t.rotation,t.crop_left,t.crop_top,t.crop_right,t.crop_bottom,v.volume,v.chroma.similarity,v.chroma.smoothness,v.chroma.spill})if(!std::isfinite(f))throw std::invalid_argument("无效素材参数");if(t.width<=0||t.height<=0||t.width>8||t.height>8||std::abs(t.x)>8||std::abs(t.y)>8||t.crop_left<0||t.crop_right<0||t.crop_top<0||t.crop_bottom<0||t.crop_left+t.crop_right>=1||t.crop_top+t.crop_bottom>=1||v.volume<0||v.volume>4)throw std::invalid_argument("素材变换或音量超出范围");for(float f:v.color)if(!std::isfinite(f)||f<0||f>1)throw std::invalid_argument("无效颜色");}}
void append(std::deque<float>&q,const std::vector<float>&p){q.insert(q.end(),p.begin(),p.end());while(q.size()>19200)q.pop_front();}
}
struct Studio::Impl : StudioGraphState {
 std::atomic<bool> stop{false};
 std::jthread worker;
 explicit Impl(StudioOptions options):StudioGraphState(options),worker([this]{run();}){}
 ~Impl(){
  stop=true;if(worker.joinable())worker.join();
  programVideo->close();localVideo->close();programAudio->close();localAudio->close();
 }
 void run(){
#ifdef _WIN32
  HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
#endif
  try{
#ifdef _WIN32
   if(FAILED(com))throw std::runtime_error("Studio COM initialization failed");
#endif
   StudioGraph graph(*this);
   graph.start();
   while(!stop){graph.step();std::this_thread::sleep_for(std::chrono::milliseconds(2));}
   graph.stop();
   // Graph-owned D3D, captures and recorder are released before CoUninitialize.
  }catch(const std::exception& e){std::lock_guard lock(mutex);status.error=e.what();}
  {std::lock_guard lock(mutex);status.recording=false;}
#ifdef _WIN32
  if(SUCCEEDED(com))CoUninitialize();
#endif
 }
};
Studio::Studio(StudioOptions o):impl_(std::make_unique<Impl>(o)){}Studio::~Studio()=default;
void Studio::set_scene(StudioScene s){validate(s);std::lock_guard lock(impl_->mutex);impl_->scene=std::move(s);}
void Studio::set_audio(StudioAudioSettings a){for(float v:{a.microphone_volume,a.system_volume,a.media_volume,a.remote_volume})if(!std::isfinite(v)||v<0||v>2)throw std::invalid_argument("无效混音音量");std::lock_guard lock(impl_->mutex);impl_->settings=a;}
void Studio::submit_remote_video(std::string id,const VideoFrame&f){if(f.width<1||f.height<1||f.width>3840||f.height>3840||f.stride_y<f.width||f.stride_u<(f.width+1)/2||f.stride_v<(f.width+1)/2||f.y.size()<size_t(f.stride_y)*f.height||f.u.size()<size_t(f.stride_u)*((f.height+1)/2)||f.v.size()<size_t(f.stride_v)*((f.height+1)/2))throw std::invalid_argument("无效远端视频");auto image=from_i420(f);std::lock_guard lock(impl_->mutex);if(impl_->remoteVideo.size()<4||impl_->remoteVideo.contains(id)){impl_->remoteVideo[id]=std::move(image);impl_->remoteSeen[id]=std::chrono::steady_clock::now();}}
void Studio::submit_remote_audio(std::string id,const AudioFrame&f){if(f.sample_rate<=0||f.channels<1||f.channels>2)return;size_t count=f.format==AudioSampleFormat::pcm16?f.samples.size():f.float_samples.size();if(count<f.samples_per_channel*f.channels)return;size_t n=f.samples_per_channel*48000/f.sample_rate;if(n>48000)return;std::vector<float>samples(n*2);for(size_t i=0;i<n;++i)for(size_t c=0;c<2;++c){size_t p=std::min(f.samples_per_channel-1,i*size_t(f.sample_rate)/48000)*f.channels+std::min(c,f.channels-1);samples[i*2+c]=f.format==AudioSampleFormat::pcm16?f.samples[p]/32768.f:f.float_samples[p];}std::lock_guard lock(impl_->mutex);if(impl_->remoteAudio.size()<4||impl_->remoteAudio.contains(id)){append(impl_->remoteAudio[id],samples);impl_->remoteSeen[id]=std::chrono::steady_clock::now();}}
void Studio::remove_remote(const std::string&id){std::lock_guard lock(impl_->mutex);impl_->remoteVideo.erase(id);impl_->remoteAudio.erase(id);impl_->remoteSeen.erase(id);}
void Studio::clear_remote(){std::lock_guard lock(impl_->mutex);impl_->remoteVideo.clear();impl_->remoteAudio.clear();impl_->remoteSeen.clear();}
void Studio::start_recording(std::filesystem::path p){if(p.extension()!=L".mp4")throw std::invalid_argument("录制文件需要 .mp4 扩展名");std::lock_guard lock(impl_->mutex);if(impl_->recordWanted||impl_->status.recording)throw std::runtime_error("正在录制");impl_->status.error.clear();impl_->recordPath=std::move(p);impl_->recordWanted=true;}
void Studio::stop_recording(){std::lock_guard lock(impl_->mutex);impl_->recordWanted=false;}
StudioSnapshot Studio::snapshot()const{std::lock_guard lock(impl_->mutex);return impl_->status;}
StudioOptions Studio::options()const{return impl_->options;}
std::shared_ptr<I420VideoSource>Studio::video_output()const{return impl_->programVideo;}std::shared_ptr<PcmAudioSource>Studio::audio_output()const{return impl_->programAudio;}
std::shared_ptr<I420VideoSource>Studio::local_video_output()const{return impl_->localVideo;}std::shared_ptr<PcmAudioSource>Studio::local_audio_output()const{return impl_->localAudio;}
void Studio::save_project(const std::filesystem::path&p,const std::vector<StudioScene>&scenes,size_t selected,StudioOptions o,StudioAudioSettings a){
 if(scenes.empty()||scenes.size()>20||selected>=scenes.size())throw std::invalid_argument("无效场景集合");Json j{{"version",1},{"selected",selected},{"width",o.width},{"height",o.height},{"audio",{{"microphone",a.microphone},{"system",a.system},{"monitor",a.monitor},{"mic_volume",a.microphone_volume},{"system_volume",a.system_volume},{"media_volume",a.media_volume},{"remote_volume",a.remote_volume}}},{"scenes",Json::array()}};
 for(auto&scene:scenes){validate(scene);Json items=Json::array();for(auto&s:scene.sources){if(s.kind==StudioSourceKind::remote)continue;auto&t=s.transform;items.push_back({{"id",s.id},{"name",s.name},{"kind",int(s.kind)},{"input",s.input},{"transform",{t.x,t.y,t.width,t.height,t.rotation,t.crop_left,t.crop_top,t.crop_right,t.crop_bottom}},{"color",s.color},{"visible",s.visible},{"locked",s.locked},{"loop",s.loop},{"muted",s.muted},{"volume",s.volume},{"chroma",{{"enabled",s.chroma.enabled},{"color",s.chroma.color},{"similarity",s.chroma.similarity},{"smoothness",s.chroma.smoothness},{"spill",s.chroma.spill}}}});}j["scenes"].push_back({{"id",scene.id},{"name",scene.name},{"sources",items}});}
 if(!p.parent_path().empty())std::filesystem::create_directories(p.parent_path());auto tmp=p;tmp+=L".tmp";{std::ofstream f(tmp,std::ios::binary|std::ios::trunc);if(!f||!(f<<j.dump(2)))throw std::runtime_error("无法保存场景");f.flush();if(!f)throw std::runtime_error("场景写入失败");}
#ifdef _WIN32
 if(!MoveFileExW(tmp.c_str(),p.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("无法替换场景文件");
#else
 std::filesystem::rename(tmp,p);
#endif
}
std::vector<StudioScene>Studio::load_project(const std::filesystem::path&p,size_t&selected,StudioOptions&o,StudioAudioSettings&a){
 if(std::filesystem::file_size(p)>2*1024*1024)throw std::runtime_error("场景文件过大");std::ifstream f(p);auto j=Json::parse(f);if(j.at("version")!=1)throw std::runtime_error("不支持的场景版本");std::vector<StudioScene>out;for(auto&v:j.at("scenes")){StudioScene scene;scene.id=v.at("id");scene.name=v.at("name");for(auto&x:v.at("sources")){StudioSource s;s.id=x.at("id");s.name=x.at("name");int kind=x.at("kind");if(kind<0||kind>=int(StudioSourceKind::remote))throw std::runtime_error("无效素材类型");s.kind=StudioSourceKind(kind);s.input=x.at("input");auto t=x.at("transform").get<std::array<float,9>>();s.transform={t[0],t[1],t[2],t[3],t[4],t[5],t[6],t[7],t[8]};s.color=x.at("color").get<std::array<float,4>>();s.visible=x.at("visible");s.locked=x.at("locked");s.loop=x.at("loop");s.muted=x.at("muted");s.volume=x.at("volume");auto&k=x.at("chroma");s.chroma.enabled=k.at("enabled");s.chroma.color=k.at("color").get<std::array<float,3>>();s.chroma.similarity=k.at("similarity");s.chroma.smoothness=k.at("smoothness");s.chroma.spill=k.at("spill");scene.sources.push_back(s);}validate(scene);out.push_back(scene);}auto sel=j.at("selected").get<size_t>();if(out.empty()||out.size()>20||sel>=out.size())throw std::runtime_error("无效场景集合");StudioAudioSettings audio;auto&v=j.at("audio");audio.microphone=v.at("microphone");audio.system=v.at("system");audio.monitor=v.at("monitor");audio.microphone_volume=v.at("mic_volume");audio.system_volume=v.at("system_volume");audio.media_volume=v.at("media_volume");audio.remote_volume=v.at("remote_volume");int width=j.at("width"),height=j.at("height");if((width!=1280||height!=720)&&(width!=720||height!=1280)&&(width!=1920||height!=1080)&&(width!=1080||height!=1920))throw std::runtime_error("无效画布尺寸");for(float gain:{audio.microphone_volume,audio.system_volume,audio.media_volume,audio.remote_volume})if(!std::isfinite(gain)||gain<0||gain>2)throw std::runtime_error("无效音频配置");o.width=width;o.height=height;a=audio;selected=sel;return out;
}
}
