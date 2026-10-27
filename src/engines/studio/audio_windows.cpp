#include "internal.hpp"
#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <wrl.h>
#include <ksmedia.h>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cmath>
extern "C" {
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
}
namespace livekit::sdk::studio_detail {
using namespace Microsoft::WRL;
static void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("音频设备错误: "+std::to_string(uint32_t(hr)));}
struct Activation : RuntimeClass<RuntimeClassFlags<ClassicCom>,IActivateAudioInterfaceCompletionHandler,FtmBase> {
 HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);ComPtr<IAudioClient>client;HRESULT result=E_FAIL;
 ~Activation(){CloseHandle(event);}
 HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*op)override{ComPtr<IUnknown>p;HRESULT activation=E_FAIL;result=op->GetActivateResult(&activation,&p);if(SUCCEEDED(result))result=activation;if(SUCCEEDED(result))result=p.As(&client);SetEvent(event);return S_OK;}
};
struct AudioCapture::Impl {
 std::mutex mutex;std::deque<float>queue;std::string error;std::atomic<bool>stop{false};std::jthread worker;
 explicit Impl(bool system){worker=std::jthread([this,system]{
  HRESULT initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);SwrContext*swr=nullptr;ComPtr<IAudioClient>client;WAVEFORMATEX*allocated=nullptr;
  try{
   check(initialized);WAVEFORMATEX format{};WAVEFORMATEX*mix=&format;
   if(system){auto completion=Make<Activation>();AUDIOCLIENT_ACTIVATION_PARAMS params{};params.ActivationType=AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;params.ProcessLoopbackParams.TargetProcessId=GetCurrentProcessId();params.ProcessLoopbackParams.ProcessLoopbackMode=PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;PROPVARIANT prop{};prop.vt=VT_BLOB;prop.blob.cbSize=sizeof(params);prop.blob.pBlobData=reinterpret_cast<BYTE*>(&params);ComPtr<IActivateAudioInterfaceAsyncOperation>op;check(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,__uuidof(IAudioClient),&prop,completion.Get(),&op));
    if(WaitForSingleObject(completion->event,5000)!=WAIT_OBJECT_0)throw std::runtime_error("系统音频初始化超时");check(completion->result);client=completion->client;format.wFormatTag=WAVE_FORMAT_IEEE_FLOAT;format.nChannels=2;format.nSamplesPerSec=48000;format.wBitsPerSample=32;format.nBlockAlign=8;format.nAvgBytesPerSec=384000;
   }else{ComPtr<IMMDeviceEnumerator>enumerator;check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)));ComPtr<IMMDevice>device;check(enumerator->GetDefaultAudioEndpoint(eCapture,eCommunications,&device));check(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,&client));check(client->GetMixFormat(&allocated));mix=allocated;}
   DWORD flags=system?AUDCLNT_STREAMFLAGS_LOOPBACK:0;check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,flags,2000000,0,mix,nullptr));
   bool floating=mix->wFormatTag==WAVE_FORMAT_IEEE_FLOAT;if(mix->wFormatTag==WAVE_FORMAT_EXTENSIBLE)floating=reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat==KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
   AVSampleFormat input=floating?AV_SAMPLE_FMT_FLT:(mix->wBitsPerSample==16?AV_SAMPLE_FMT_S16:AV_SAMPLE_FMT_S32);if(mix->wBitsPerSample!=16&&mix->wBitsPerSample!=32)throw std::runtime_error("不支持的音频设备格式");AVChannelLayout in{},out=AV_CHANNEL_LAYOUT_STEREO;av_channel_layout_default(&in,mix->nChannels);int e=swr_alloc_set_opts2(&swr,&out,AV_SAMPLE_FMT_FLT,48000,&in,input,mix->nSamplesPerSec,0,nullptr);av_channel_layout_uninit(&in);if(e<0||swr_init(swr)<0)throw std::runtime_error("音频重采样初始化失败");
   ComPtr<IAudioCaptureClient>capture;check(client->GetService(IID_PPV_ARGS(&capture)));check(client->Start());
   while(!stop){UINT32 available=0;check(capture->GetNextPacketSize(&available));if(!available){std::this_thread::sleep_for(std::chrono::milliseconds(3));continue;}BYTE*data=nullptr;UINT32 frames=0;DWORD status=0;check(capture->GetBuffer(&data,&frames,&status,nullptr,nullptr));std::vector<uint8_t>silence;if(status&AUDCLNT_BUFFERFLAGS_SILENT){silence.resize(size_t(frames)*mix->nBlockAlign);data=silence.data();}
    int cap=swr_get_out_samples(swr,frames);std::vector<float>samples(size_t(cap)*2);uint8_t*dst=reinterpret_cast<uint8_t*>(samples.data());const uint8_t*src=data;int count=swr_convert(swr,&dst,cap,&src,frames);capture->ReleaseBuffer(frames);if(count<0)throw std::runtime_error("音频转换失败");samples.resize(size_t(count)*2);{std::lock_guard lock(mutex);queue.insert(queue.end(),samples.begin(),samples.end());while(queue.size()>19200)queue.pop_front();}
   }
  }catch(const std::exception&e){std::lock_guard lock(mutex);error=e.what();}
  if(client)client->Stop();if(allocated)CoTaskMemFree(allocated);swr_free(&swr);if(SUCCEEDED(initialized))CoUninitialize();
 });}
 ~Impl(){stop=true;if(worker.joinable())worker.join();}
};
AudioCapture::AudioCapture(bool system):impl_(std::make_unique<Impl>(system)){}AudioCapture::~AudioCapture()=default;
std::vector<float>AudioCapture::take(size_t frames){std::lock_guard lock(impl_->mutex);std::vector<float>out(frames*2);size_t n=std::min(out.size(),impl_->queue.size());for(size_t i=0;i<n;++i){out[i]=impl_->queue.front();impl_->queue.pop_front();}return out;}
std::string AudioCapture::error(){std::lock_guard lock(impl_->mutex);return impl_->error;}
// Playback uses a separate render worker and a bounded queue. The process
// loopback capture excludes this process, including this monitor stream.
struct Monitor::Impl{
 std::mutex mutex;std::deque<float>queue;std::string error;std::atomic<bool>stop{false};std::jthread worker;
 Impl(){worker=std::jthread([this]{HRESULT initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);ComPtr<IAudioClient>client;try{check(initialized);ComPtr<IMMDeviceEnumerator>en;check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&en)));ComPtr<IMMDevice>dev;check(en->GetDefaultAudioEndpoint(eRender,eMultimedia,&dev));check(dev->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,&client));WAVEFORMATEX f{};f.wFormatTag=WAVE_FORMAT_IEEE_FLOAT;f.nChannels=2;f.nSamplesPerSec=48000;f.wBitsPerSample=32;f.nBlockAlign=8;f.nAvgBytesPerSec=384000;check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,1000000,0,&f,nullptr));ComPtr<IAudioRenderClient>render;check(client->GetService(IID_PPV_ARGS(&render)));UINT32 capacity;check(client->GetBufferSize(&capacity));check(client->Start());while(!stop){UINT32 used=0;check(client->GetCurrentPadding(&used));UINT32 count=std::min(UINT32(480),capacity-used);if(count){BYTE*data=nullptr;check(render->GetBuffer(count,&data));auto*p=reinterpret_cast<float*>(data);std::lock_guard lock(mutex);for(UINT32 i=0;i<count*2;++i){p[i]=queue.empty()?0:queue.front();if(!queue.empty())queue.pop_front();}check(render->ReleaseBuffer(count,0));}std::this_thread::sleep_for(std::chrono::milliseconds(5));}}catch(const std::exception&e){std::lock_guard lock(mutex);error=e.what();}if(client)client->Stop();if(SUCCEEDED(initialized))CoUninitialize();});}
 ~Impl(){stop=true;if(worker.joinable())worker.join();}
};
Monitor::Monitor():impl_(std::make_unique<Impl>()){}Monitor::~Monitor()=default;void Monitor::write(std::span<const float>s){std::lock_guard lock(impl_->mutex);impl_->queue.insert(impl_->queue.end(),s.begin(),s.end());while(impl_->queue.size()>9600)impl_->queue.pop_front();}std::string Monitor::error(){std::lock_guard lock(impl_->mutex);return impl_->error;}void Monitor::clear(){std::lock_guard lock(impl_->mutex);impl_->queue.clear();}
}
