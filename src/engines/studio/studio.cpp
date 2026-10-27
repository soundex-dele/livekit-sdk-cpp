#include "internal.hpp"
#include "livekit/player.hpp"
#include <windows.h>
#include <objbase.h>
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
float peak(const std::vector<float>&samples){float p=0;for(float v:samples)p=std::max(p,std::abs(v));return p;}
std::vector<float>take(std::deque<float>&q,size_t n=960){std::vector<float>out(n);for(size_t i=0;i<n&&!q.empty();++i){out[i]=q.front();q.pop_front();}return out;}
void append(std::deque<float>&q,const std::vector<float>&p){q.insert(q.end(),p.begin(),p.end());while(q.size()>19200)q.pop_front();}
PcmSourceOptions audioOptions(){PcmSourceOptions o;o.channels=2;o.max_queue_duration=std::chrono::milliseconds(60);o.format=AudioSampleFormat::float32;return o;}
}
struct Studio::Impl {
 StudioOptions options;mutable std::mutex mutex;StudioScene scene;StudioAudioSettings settings;StudioSnapshot status;
 std::map<std::string,std::chrono::steady_clock::time_point>remoteSeen;std::map<std::string,std::shared_ptr<const StudioImage>>remoteVideo;std::map<std::string,std::deque<float>>remoteAudio;
 std::shared_ptr<I420VideoSource>programVideo,localVideo;std::shared_ptr<PcmAudioSource>programAudio,localAudio;
 bool recordWanted=false;std::filesystem::path recordPath;std::atomic<bool>stop{false};std::jthread worker;
 struct Source {StudioSource spec;std::unique_ptr<Capture>capture;std::unique_ptr<Player>player;std::shared_ptr<const StudioImage>image;std::deque<float>audio;std::string error;std::chrono::steady_clock::time_point attempt=std::chrono::steady_clock::now();};
 explicit Impl(StudioOptions o):options(o){if((o.width!=1280||o.height!=720)&&(o.width!=720||o.height!=1280)&&(o.width!=1920||o.height!=1080)&&(o.width!=1080||o.height!=1920))throw std::invalid_argument("请选择 720p 或 1080p 横竖屏");if(o.fps!=30)throw std::invalid_argument("首版帧率为 30 fps");I420SourceOptions v;v.width=o.width;v.height=o.height;programVideo=std::make_shared<I420VideoSource>(v);localVideo=std::make_shared<I420VideoSource>(v);programAudio=std::make_shared<PcmAudioSource>(audioOptions());localAudio=std::make_shared<PcmAudioSource>(audioOptions());worker=std::jthread([this]{run();});}
 ~Impl(){stop=true;if(worker.joinable())worker.join();programVideo->close();localVideo->close();programAudio->close();localAudio->close();}
 void run(){HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);std::unique_ptr<Recorder>recorder;try{
  Compositor compositor(options.width,options.height,options.software_gpu);std::map<std::string,Source>sources;std::unique_ptr<AudioCapture>mic,system;std::unique_ptr<Monitor>monitor;auto nextAudio=std::chrono::steady_clock::now(),nextVideo=nextAudio,deviceRetry=nextAudio;auto videoStep=std::chrono::microseconds(1000000/options.fps);
  while(!stop){StudioScene current;StudioAudioSettings audio;bool recording;std::filesystem::path path;std::map<std::string,std::shared_ptr<const StudioImage>>remote;
   {std::lock_guard lock(mutex);current=scene;audio=settings;recording=recordWanted;path=recordPath;auto now=std::chrono::steady_clock::now();std::erase_if(remoteSeen,[&](const auto&e){if(now-e.second<std::chrono::seconds(5))return false;remoteVideo.erase(e.first);remoteAudio.erase(e.first);return true;});remote=remoteVideo;}
   if(recording&&!recorder){try{recorder=std::make_unique<Recorder>(path,options.width,options.height,options.fps);std::lock_guard lock(mutex);status.recording=true;status.recording_path=path.string();}catch(const std::exception&e){std::lock_guard lock(mutex);status.error=e.what();recordWanted=false;}}
   if(!recording&&recorder){try{recorder->finish();}catch(const std::exception&e){std::lock_guard lock(mutex);status.error=e.what();}recorder.reset();std::lock_guard lock(mutex);status.recording=false;}
   if(std::chrono::steady_clock::now()>=deviceRetry){deviceRetry=std::chrono::steady_clock::now()+std::chrono::seconds(5);if(mic&&!mic->error().empty())mic.reset();if(system&&!system->error().empty())system.reset();if(monitor&&!monitor->error().empty())monitor.reset();}
   if(audio.microphone&&!mic)mic=std::make_unique<AudioCapture>(false);if(!audio.microphone)mic.reset();if(audio.system&&!system)system=std::make_unique<AudioCapture>(true);if(!audio.system)system.reset();if(audio.monitor&&!monitor)monitor=std::make_unique<Monitor>();if(!audio.monitor)monitor.reset();
   std::set<std::string>active;std::map<std::string,std::string>errors;if(monitor&&!monitor->error().empty())errors["monitor"]=monitor->error();
   for(const auto&s:current.sources){if(s.kind==StudioSourceKind::remote||!s.visible)continue;active.insert(s.id);auto it=sources.find(s.id);if(it!=sources.end()&&(it->second.spec.kind!=s.kind||it->second.spec.input!=s.input||it->second.spec.color!=s.color||(!it->second.error.empty()&&std::chrono::steady_clock::now()-it->second.attempt>std::chrono::seconds(5))))sources.erase(it);
    auto[entry,inserted]=sources.try_emplace(s.id);auto&source=entry->second;auto previousError=source.error;
    if(inserted){source.spec=s;try{if(s.kind==StudioSourceKind::camera||s.kind==StudioSourceKind::screen||s.kind==StudioSourceKind::window)source.capture=std::make_unique<Capture>(s);else if(s.kind==StudioSourceKind::image)source.image=load_image(s.input);else if(s.kind==StudioSourceKind::text)source.image=text_image(s.input,s.color);else if(s.kind==StudioSourceKind::video){source.player=std::make_unique<Player>();source.player->open(s.input).get();source.player->play().get();}else{auto img=std::make_shared<StudioImage>();img->width=img->height=2;img->rgba.resize(16);for(int p=0;p<4;++p)for(int c=0;c<4;++c)img->rgba[p*4+c]=uint8_t(s.color[c]*255);source.image=img;}}catch(const std::exception&e){source.error=e.what();}}
    source.spec=s;if(source.capture){source.error=source.capture->error();if(source.error.empty())source.image=source.capture->frame();}
    if(source.player){while(auto frame=source.player->poll_video()){auto img=std::make_shared<StudioImage>();img->width=frame->width;img->height=frame->height;img->rgba.resize(size_t(img->width)*img->height*4);for(int y=0;y<img->height;++y)memcpy(img->rgba.data()+size_t(y)*img->width*4,frame->rgba->data()+size_t(y)*frame->stride,size_t(img->width)*4);source.image=img;}while(auto frame=source.player->poll_audio())append(source.audio,*frame->pcm);auto p=source.player->snapshot();if(p.state==PlayerState::ended&&s.loop){source.player->seek(std::chrono::microseconds(0)).get();source.player->play().get();}if(!p.error.empty())source.error=p.error;}
    if(!source.error.empty()){if(source.error!=previousError)source.image.reset();errors[s.id]=source.error;if(!source.image)source.image=text_image("素材不可用 · 正在重试",{.7f,.72f,.78f,1});}
   }
   std::erase_if(sources,[&](auto&entry){return !active.contains(entry.first);});
   auto now=std::chrono::steady_clock::now();int catchup=0;
   while(now>=nextAudio&&catchup++<10){std::vector<float>mixed(960),micSamples(960),systemSamples(960);if(mic){micSamples=mic->take(480);if(!mic->error().empty())errors["microphone"]=mic->error();}if(system){systemSamples=system->take(480);if(!system->error().empty())errors["system"]=system->error();}studio_mix(mixed,micSamples,audio.microphone_volume);studio_mix(mixed,systemSamples,audio.system_volume);for(auto&[id,s]:sources){auto samples=take(s.audio);if(!s.spec.muted)studio_mix(mixed,samples,audio.media_volume*s.spec.volume);}for(auto&v:mixed)v=std::clamp(v,-1.f,1.f);PcmInputFrame input;input.capture_time=nextAudio;input.float_samples=mixed;localAudio->write(input);std::vector<float>returnAudio(960);{std::lock_guard lock(mutex);for(auto&[id,q]:remoteAudio)studio_mix(returnAudio,take(q),audio.remote_volume);}studio_mix(mixed,returnAudio,1);for(auto&v:mixed)v=std::clamp(v,-1.f,1.f);input.float_samples=mixed;programAudio->write(input);if(monitor)monitor->write(returnAudio);
    if(recorder){try{recorder->audio(mixed,nextAudio);}catch(const std::exception&e){std::lock_guard lock(mutex);status.error=e.what();recordWanted=false;}}
    {std::lock_guard lock(mutex);status.microphone_peak=peak(micSamples)*audio.microphone_volume;status.system_peak=peak(systemSamples)*audio.system_volume;status.output_peak=peak(mixed);}nextAudio+=std::chrono::milliseconds(10);
   }
   if(now-nextAudio>std::chrono::milliseconds(100))nextAudio=now;
   if(now>=nextVideo){Layers local,program;for(auto&s:current.sources){std::shared_ptr<const StudioImage>image;if(s.kind==StudioSourceKind::remote){auto it=remote.find(s.input);if(it!=remote.end())image=it->second;}else{auto it=sources.find(s.id);if(it!=sources.end())image=it->second.image;}if(image){program.emplace_back(s,image);if(s.kind!=StudioSourceKind::remote)local.emplace_back(s,image);}}
    auto localImage=std::make_shared<StudioImage>(compositor.render(local));
    // Interactive output uses a clean full-canvas local feed; return feeds are
    // placed only in the program. This also makes PK layouts non-recursive.
    bool explicitRemote=std::any_of(current.sources.begin(),current.sources.end(),[](const auto&s){return s.kind==StudioSourceKind::remote;});
    if(!remote.empty()&&!explicitRemote){program.clear();StudioSource base;base.id="program-local";base.transform={0,0,.5f,1};program.emplace_back(base,localImage);int i=0;for(auto&[id,image]:remote){StudioSource tile;tile.id=id;tile.transform={.5f,float(i)/float(remote.size()),.5f,1.f/float(remote.size())};program.emplace_back(tile,image);++i;}}
    auto composed=(remote.empty()&&local.size()==program.size())?localImage:std::make_shared<StudioImage>(compositor.render(program));auto video=to_i420(*composed);auto result=programVideo->write(video);localVideo->write(composed==localImage?video:to_i420(*localImage));
    if(recorder){try{recorder->video(*composed,now);}catch(const std::exception&e){std::lock_guard lock(mutex);status.error=e.what();recordWanted=false;}}
    {std::lock_guard lock(mutex);status.preview=composed;status.frames++;if(result==VideoWriteStatus::backpressure&&programVideo->stats().attached)status.dropped++;status.source_errors=errors;}nextVideo+=videoStep;if(now-nextVideo>videoStep){std::lock_guard lock(mutex);status.dropped+=uint64_t((now-nextVideo)/videoStep);nextVideo=now+videoStep;}
   }
   std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
 }catch(const std::exception&e){std::lock_guard lock(mutex);status.error=e.what();}
 if(recorder){try{recorder->finish();}catch(const std::exception&e){std::lock_guard lock(mutex);status.error=e.what();}}
 {std::lock_guard lock(mutex);status.recording=false;}if(SUCCEEDED(com))CoUninitialize();
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
 if(!p.parent_path().empty())std::filesystem::create_directories(p.parent_path());auto tmp=p;tmp+=L".tmp";{std::ofstream f(tmp,std::ios::binary|std::ios::trunc);if(!f||!(f<<j.dump(2)))throw std::runtime_error("无法保存场景");f.flush();if(!f)throw std::runtime_error("场景写入失败");}if(!MoveFileExW(tmp.c_str(),p.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("无法替换场景文件");
}
std::vector<StudioScene>Studio::load_project(const std::filesystem::path&p,size_t&selected,StudioOptions&o,StudioAudioSettings&a){
 if(std::filesystem::file_size(p)>2*1024*1024)throw std::runtime_error("场景文件过大");std::ifstream f(p);auto j=Json::parse(f);if(j.at("version")!=1)throw std::runtime_error("不支持的场景版本");std::vector<StudioScene>out;for(auto&v:j.at("scenes")){StudioScene scene;scene.id=v.at("id");scene.name=v.at("name");for(auto&x:v.at("sources")){StudioSource s;s.id=x.at("id");s.name=x.at("name");int kind=x.at("kind");if(kind<0||kind>=int(StudioSourceKind::remote))throw std::runtime_error("无效素材类型");s.kind=StudioSourceKind(kind);s.input=x.at("input");auto t=x.at("transform").get<std::array<float,9>>();s.transform={t[0],t[1],t[2],t[3],t[4],t[5],t[6],t[7],t[8]};s.color=x.at("color").get<std::array<float,4>>();s.visible=x.at("visible");s.locked=x.at("locked");s.loop=x.at("loop");s.muted=x.at("muted");s.volume=x.at("volume");auto&k=x.at("chroma");s.chroma.enabled=k.at("enabled");s.chroma.color=k.at("color").get<std::array<float,3>>();s.chroma.similarity=k.at("similarity");s.chroma.smoothness=k.at("smoothness");s.chroma.spill=k.at("spill");scene.sources.push_back(s);}validate(scene);out.push_back(scene);}auto sel=j.at("selected").get<size_t>();if(out.empty()||out.size()>20||sel>=out.size())throw std::runtime_error("无效场景集合");StudioAudioSettings audio;auto&v=j.at("audio");audio.microphone=v.at("microphone");audio.system=v.at("system");audio.monitor=v.at("monitor");audio.microphone_volume=v.at("mic_volume");audio.system_volume=v.at("system_volume");audio.media_volume=v.at("media_volume");audio.remote_volume=v.at("remote_volume");int width=j.at("width"),height=j.at("height");if((width!=1280||height!=720)&&(width!=720||height!=1280)&&(width!=1920||height!=1080)&&(width!=1080||height!=1920))throw std::runtime_error("无效画布尺寸");for(float gain:{audio.microphone_volume,audio.system_volume,audio.media_volume,audio.remote_volume})if(!std::isfinite(gain)||gain<0||gain>2)throw std::runtime_error("无效音频配置");o.width=width;o.height=height;a=audio;selected=sel;return out;
}
}
