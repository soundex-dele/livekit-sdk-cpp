#include "internal.hpp"
#include "media_nodes/capture/capture.hpp"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <wrl/client.h>
#include <thread>
#include <atomic>
#include <algorithm>

namespace livekit::sdk::studio_detail {
using Microsoft::WRL::ComPtr;
using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;
static std::wstring wide(const std::string&s){return std::filesystem::u8path(s).wstring();}
struct Capture::Impl {
 struct State {std::mutex mutex;std::shared_ptr<const StudioImage> image;std::string error;};
 std::shared_ptr<State> state=std::make_shared<State>();std::atomic<bool>stop{false};std::jthread worker;
 explicit Impl(StudioSource s){worker=std::jthread([this,s]{
  bool apartment=false;try{
   init_apartment(apartment_type::multi_threaded);apartment=true;
   if(s.kind==StudioSourceKind::camera){MediaSpec spec;spec.capture=CaptureKind::camera;spec.device_index=std::stoi(s.input);spec.width=1280;spec.height=720;spec.fps=30;
    lk::capture_device(spec,stop,[p=state](const webrtc::VideoFrame&v){auto b=v.video_frame_buffer()->ToI420();VideoFrame f;f.width=b->width();f.height=b->height();f.stride_y=b->StrideY();f.stride_u=b->StrideU();f.stride_v=b->StrideV();f.y.assign(b->DataY(),b->DataY()+size_t(f.stride_y)*f.height);f.u.assign(b->DataU(),b->DataU()+size_t(f.stride_u)*((f.height+1)/2));f.v.assign(b->DataV(),b->DataV()+size_t(f.stride_v)*((f.height+1)/2));auto img=from_i420(f);std::lock_guard lock(p->mutex);p->image=std::move(img);});
   }else{
    ComPtr<ID3D11Device>device;ComPtr<ID3D11DeviceContext>context;check_hresult(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));ComPtr<IDXGIDevice>dxgi;check_hresult(device.As(&dxgi));com_ptr<IInspectable>inspectable;check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(),inspectable.put()));auto direct=inspectable.as<IDirect3DDevice>();
    auto interop=get_activation_factory<GraphicsCaptureItem,IGraphicsCaptureItemInterop>();GraphicsCaptureItem item{nullptr};
    if(s.kind==StudioSourceKind::window){auto hwnd=reinterpret_cast<HWND>(std::stoull(s.input));if(!IsWindow(hwnd))throw std::runtime_error("窗口已关闭");check_hresult(interop->CreateForWindow(hwnd,guid_of<GraphicsCaptureItem>(),put_abi(item)));}
    else{HMONITOR monitor=nullptr;auto target=wide(s.input);struct Find {std::wstring target;HMONITOR found=nullptr;}find{target};EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR m,HDC,LPRECT,LPARAM data)->BOOL{auto&f=*reinterpret_cast<Find*>(data);MONITORINFOEXW info{};info.cbSize=sizeof(info);GetMonitorInfoW(m,&info);if(f.target.empty()?(info.dwFlags&MONITORINFOF_PRIMARY):f.target==info.szDevice){f.found=m;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&find));monitor=find.found;if(!monitor)throw std::runtime_error("显示器不可用");check_hresult(interop->CreateForMonitor(monitor,guid_of<GraphicsCaptureItem>(),put_abi(item)));}
    auto pool=Direct3D11CaptureFramePool::CreateFreeThreaded(direct,DirectXPixelFormat::B8G8R8A8UIntNormalized,2,item.Size());auto session=pool.CreateCaptureSession(item);auto size=item.Size();
    auto arrived=pool.FrameArrived(auto_revoke,[p=state,device,context](const Direct3D11CaptureFramePool&sender,auto&&){try{auto frame=sender.TryGetNextFrame();if(!frame)return;auto access=frame.Surface().as<IDirect3DDxgiInterfaceAccess>();ComPtr<ID3D11Texture2D>texture;check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D>staging;check_hresult(device->CreateTexture2D(&d,nullptr,&staging));context->CopyResource(staging.Get(),texture.Get());D3D11_MAPPED_SUBRESOURCE map{};check_hresult(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));auto img=std::make_shared<StudioImage>();img->width=std::min(int(d.Width),frame.ContentSize().Width);img->height=std::min(int(d.Height),frame.ContentSize().Height);if(img->width>0&&img->height>0){img->rgba.resize(size_t(img->width)*img->height*4);for(int y=0;y<img->height;++y){auto*src=static_cast<uint8_t*>(map.pData)+size_t(y)*map.RowPitch;auto*dst=img->rgba.data()+size_t(y)*img->width*4;for(int x=0;x<img->width;++x){dst[x*4]=src[x*4+2];dst[x*4+1]=src[x*4+1];dst[x*4+2]=src[x*4];dst[x*4+3]=255;}}}context->Unmap(staging.Get(),0);std::lock_guard lock(p->mutex);p->image=std::move(img);}catch(const hresult_error&e){std::lock_guard lock(p->mutex);p->error=to_string(e.message());}catch(const std::exception&e){std::lock_guard lock(p->mutex);p->error=e.what();}});
    auto closed=item.Closed(auto_revoke,[p=state](auto&&,auto&&){std::lock_guard lock(p->mutex);p->error="采集窗口或显示器已关闭，正在重试";});
    session.StartCapture();while(!stop){auto next=item.Size();if(next.Width>0&&next.Height>0&&(next.Width!=size.Width||next.Height!=size.Height)){pool.Recreate(direct,DirectXPixelFormat::B8G8R8A8UIntNormalized,2,next);size=next;}std::this_thread::sleep_for(std::chrono::milliseconds(50));}arrived.revoke();session.Close();pool.Close();
   }
  }catch(const hresult_error&e){std::lock_guard lock(state->mutex);state->error=to_string(e.message());}catch(const std::exception&e){std::lock_guard lock(state->mutex);state->error=e.what();}if(apartment)uninit_apartment();
 });}
 ~Impl(){stop=true;if(worker.joinable())worker.join();}
};
Capture::Capture(const StudioSource&s):impl_(std::make_unique<Impl>(s)){}Capture::~Capture()=default;
std::shared_ptr<const StudioImage>Capture::frame(){std::lock_guard lock(impl_->state->mutex);return impl_->state->image;}
std::string Capture::error(){std::lock_guard lock(impl_->state->mutex);return impl_->state->error;}
std::shared_ptr<StudioImage>load_image(const std::string&path)try{
 ComPtr<IWICImagingFactory>factory;check_hresult(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));ComPtr<IWICBitmapDecoder>decoder;check_hresult(factory->CreateDecoderFromFilename(wide(path).c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder));ComPtr<IWICBitmapFrameDecode>frame;check_hresult(decoder->GetFrame(0,&frame));ComPtr<IWICFormatConverter>convert;check_hresult(factory->CreateFormatConverter(&convert));check_hresult(convert->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));UINT w,h;check_hresult(convert->GetSize(&w,&h));if(w<1||h<1||w>8192||h>8192)throw std::runtime_error("图片尺寸超出限制");auto image=std::make_shared<StudioImage>();image->width=w;image->height=h;image->rgba.resize(size_t(w)*h*4);check_hresult(convert->CopyPixels(nullptr,w*4,UINT(image->rgba.size()),image->rgba.data()));return image;
}catch(const hresult_error&e){throw std::runtime_error("图片不可用: "+to_string(e.message()));}
std::shared_ptr<StudioImage>text_image(const std::string&text,const std::array<float,4>&color){
 const int w=1280,h=180;HDC dc=CreateCompatibleDC(nullptr);if(!dc)throw std::runtime_error("无法创建文字画布");BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=w;bi.bmiHeader.biHeight=-h;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;void*pixels=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&pixels,nullptr,0);if(!bitmap){DeleteDC(dc);throw std::runtime_error("无法分配文字画布");}auto old=SelectObject(dc,bitmap);memset(pixels,0,size_t(w)*h*4);HFONT font=CreateFontW(-64,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");auto oldFont=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));RECT rect{4,4,w-4,h-4};auto str=wide(text);DrawTextW(dc,str.c_str(),int(str.size()),&rect,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);GdiFlush();auto img=std::make_shared<StudioImage>();img->width=w;img->height=h;img->rgba.resize(size_t(w)*h*4);for(size_t i=0;i<img->rgba.size();i+=4){auto*p=static_cast<uint8_t*>(pixels)+i;img->rgba[i]=uint8_t(color[0]*255);img->rgba[i+1]=uint8_t(color[1]*255);img->rgba[i+2]=uint8_t(color[2]*255);img->rgba[i+3]=uint8_t(std::max({p[0],p[1],p[2]})*color[3]);}SelectObject(dc,oldFont);DeleteObject(font);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);return img;
}
}
namespace livekit::sdk {
std::vector<StudioDevice>studio_windows(){std::vector<StudioDevice>out;EnumWindows([](HWND hwnd,LPARAM p)->BOOL{if(!IsWindowVisible(hwnd)||GetWindowTextLengthW(hwnd)==0)return TRUE;DWORD pid=0;GetWindowThreadProcessId(hwnd,&pid);if(pid==GetCurrentProcessId())return TRUE;wchar_t title[1024]{};GetWindowTextW(hwnd,title,1024);auto str=std::filesystem::path(title).u8string();reinterpret_cast<std::vector<StudioDevice>*>(p)->push_back({std::to_string(reinterpret_cast<uintptr_t>(hwnd)),std::string(str.begin(),str.end())});return TRUE;},reinterpret_cast<LPARAM>(&out));return out;}
}
