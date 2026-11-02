#include "livekit/compat/thread.hpp"
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
#include <charconv>
#include <limits>
#include <cmath>

namespace livekit::sdk::studio_detail {
using Microsoft::WRL::ComPtr;
using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

static Result<std::wstring> wide(const std::string& s) {
  if (s.empty())
    return Result<std::wstring>::success({});
  if (s.size() > size_t(INT_MAX))
    return failed("Text too long", ErrorCode::invalid_argument);
  int count =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
  if (!count)
    return failed("Invalid UTF-8", ErrorCode::invalid_argument);
  std::wstring result(size_t(count), L'\0');
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), result.data(),
                           count))
    return failed("UTF-8 conversion failed");
  return Result<std::wstring>::success(std::move(result));
}

static Result<void> check(HRESULT hr) {
  if (FAILED(hr))
    return failed("Windows capture failed: " + std::to_string(uint32_t(hr)),
                  ErrorCode::processing_failed, int(hr));
  return Result<void>::success();
}

template <class T>
Result<T> parse_id(const std::string& text) {
  T value{};
  auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size())
    return failed("Invalid capture device id", ErrorCode::invalid_argument);
  return Result<T>::success(value);
}

struct Capture::Impl {
  struct State {
    std::mutex mutex;
    std::shared_ptr<const StudioImage> image;
    std::string error;
  };

  std::shared_ptr<State> state = std::make_shared<State>();
  std::atomic<bool> stop{false};
  livekit::compat::jthread worker;

  explicit Impl(StudioSource s) {
    worker = livekit::compat::jthread([this, s] {
      bool apartment = false;
      auto run = [&]() -> Result<void> {
        init_apartment(apartment_type::multi_threaded);
        apartment = true;
        if (s.kind == StudioSourceKind::camera) {
          MediaSpec spec;
          spec.capture = CaptureKind::camera;
          LK_STUDIO_ASSIGN(index, parse_id<int>(s.input));
          if (index < 0)
            return failed("Invalid camera index", ErrorCode::invalid_argument);
          spec.device_index = index;
          spec.width = 1280;
          spec.height = 720;
          spec.fps = 30;
          auto captured = lk::capture_device(
              spec, stop, [p = state](const webrtc::VideoFrame& v) -> Result<void> {
                auto b = v.video_frame_buffer()->ToI420();
                if (!b)
                  return failed("Camera frame conversion failed");
                VideoFrame f;
                f.width = b->width();
                f.height = b->height();
                f.stride_y = b->StrideY();
                f.stride_u = b->StrideU();
                f.stride_v = b->StrideV();
                f.y.assign(b->DataY(), b->DataY() + size_t(f.stride_y) * f.height);
                f.u.assign(b->DataU(), b->DataU() + size_t(f.stride_u) * ((f.height + 1) / 2));
                f.v.assign(b->DataV(), b->DataV() + size_t(f.stride_v) * ((f.height + 1) / 2));
                LK_STUDIO_ASSIGN(img, from_i420(f));
                std::lock_guard lock(p->mutex);
                p->image = std::move(img);
                return Result<void>::success();
              });
          LK_STUDIO_TRY(captured);
        } else {
          ComPtr<ID3D11Device> device;
          ComPtr<ID3D11DeviceContext> context;
          LK_STUDIO_TRY(check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                                D3D11_SDK_VERSION, &device, nullptr, &context)));
          ComPtr<IDXGIDevice> dxgi;
          LK_STUDIO_TRY(check(device.As(&dxgi)));
          com_ptr<IInspectable> inspectable;
          LK_STUDIO_TRY(check(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put())));
          auto direct = inspectable.as<IDirect3DDevice>();
          auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
          GraphicsCaptureItem item{nullptr};
          if (s.kind == StudioSourceKind::window) {
            LK_STUDIO_ASSIGN(handle, parse_id<uintptr_t>(s.input));
            auto hwnd = reinterpret_cast<HWND>(handle);
            if (!IsWindow(hwnd))
              return failed("窗口已关闭");
            LK_STUDIO_TRY(check(
                interop->CreateForWindow(hwnd, guid_of<GraphicsCaptureItem>(), put_abi(item))));
          } else {
            HMONITOR monitor = nullptr;
            LK_STUDIO_ASSIGN(target, wide(s.input));
            struct Find {
              std::wstring target;
              HMONITOR found = nullptr;
            } find{target};
            EnumDisplayMonitors(
                nullptr, nullptr,
                [](HMONITOR m, HDC, LPRECT, LPARAM data) -> BOOL {
                  auto& f = *reinterpret_cast<Find*>(data);
                  MONITORINFOEXW info{};
                  info.cbSize = sizeof(info);
                  GetMonitorInfoW(m, &info);
                  if (f.target.empty() ? (info.dwFlags & MONITORINFOF_PRIMARY)
                                       : f.target == info.szDevice) {
                    f.found = m;
                    return FALSE;
                  }
                  return TRUE;
                },
                reinterpret_cast<LPARAM>(&find));
            monitor = find.found;
            if (!monitor)
              return failed("显示器不可用");
            LK_STUDIO_TRY(check(
                interop->CreateForMonitor(monitor, guid_of<GraphicsCaptureItem>(), put_abi(item))));
          }
          auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
              direct, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
          auto session = pool.CreateCaptureSession(item);
          auto size = item.Size();
          auto arrived = pool.FrameArrived(
              auto_revoke,
              [p = state, device, context](const Direct3D11CaptureFramePool& sender, auto&&) {
                auto capture_frame = [&]() -> Result<void> {
                  auto frame = sender.TryGetNextFrame();
                  if (!frame)
                    return Result<void>::success();
                  auto access = frame.Surface().as<IDirect3DDxgiInterfaceAccess>();
                  ComPtr<ID3D11Texture2D> texture;
                  LK_STUDIO_TRY(check(access->GetInterface(IID_PPV_ARGS(&texture))));
                  D3D11_TEXTURE2D_DESC d{};
                  texture->GetDesc(&d);
                  d.Usage = D3D11_USAGE_STAGING;
                  d.BindFlags = 0;
                  d.MiscFlags = 0;
                  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                  ComPtr<ID3D11Texture2D> staging;
                  LK_STUDIO_TRY(check(device->CreateTexture2D(&d, nullptr, &staging)));
                  context->CopyResource(staging.Get(), texture.Get());
                  D3D11_MAPPED_SUBRESOURCE map{};
                  LK_STUDIO_TRY(check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map)));
                  auto img = std::make_shared<StudioImage>();
                  img->width = std::min(int(d.Width), frame.ContentSize().Width);
                  img->height = std::min(int(d.Height), frame.ContentSize().Height);
                  if (img->width > 0 && img->height > 0) {
                    img->rgba.resize(size_t(img->width) * img->height * 4);
                    for (int y = 0; y < img->height; ++y) {
                      auto* src = static_cast<uint8_t*>(map.pData) + size_t(y) * map.RowPitch;
                      auto* dst = img->rgba.data() + size_t(y) * img->width * 4;
                      for (int x = 0; x < img->width; ++x) {
                        dst[x * 4] = src[x * 4 + 2];
                        dst[x * 4 + 1] = src[x * 4 + 1];
                        dst[x * 4 + 2] = src[x * 4];
                        dst[x * 4 + 3] = 255;
                      }
                    }
                  }
                  context->Unmap(staging.Get(), 0);
                  std::lock_guard lock(p->mutex);
                  p->image = std::move(img);
                  return Result<void>::success();
                };
                Result<void> result = Result<void>::success();
                try {
                  result = capture_frame();
                } catch (const hresult_error& e) {
                  result =
                      failed(to_string(e.message()), ErrorCode::processing_failed, int(e.code()));
                }
                if (!result) {
                  std::lock_guard lock(p->mutex);
                  p->error = result.error().message;
                }
              });
          auto closed = item.Closed(auto_revoke, [p = state](auto&&, auto&&) {
            std::lock_guard lock(p->mutex);
            p->error = "采集窗口或显示器已关闭，正在重试";
          });
          session.StartCapture();
          while (!stop) {
            auto next = item.Size();
            if (next.Width > 0 && next.Height > 0 &&
                (next.Width != size.Width || next.Height != size.Height)) {
              pool.Recreate(direct, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, next);
              size = next;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          }
          arrived.revoke();
          session.Close();
          pool.Close();
        }
        return Result<void>::success();
      };
      Result<void> result = Result<void>::success();
      try {
        result = run();
      } catch (const hresult_error& e) {
        result = failed(to_string(e.message()), ErrorCode::processing_failed, int(e.code()));
      }
      if (!result) {
        std::lock_guard lock(state->mutex);
        state->error = result.error().message;
      }
      if (apartment)
        uninit_apartment();
    });
  }

  ~Impl() {
    stop = true;
    if (worker.joinable())
      worker.join();
  }
};

Capture::Capture(const StudioSource& s) : impl_(std::make_unique<Impl>(s)) {}

Capture::~Capture() = default;

std::shared_ptr<const StudioImage> Capture::frame() {
  std::lock_guard lock(impl_->state->mutex);
  return impl_->state->image;
}

std::string Capture::error() {
  std::lock_guard lock(impl_->state->mutex);
  return impl_->state->error;
}

Result<std::shared_ptr<StudioImage>> load_image(const std::string& path) {
  LK_STUDIO_ASSIGN(filename, wide(path));
  ComPtr<IWICImagingFactory> factory;
  LK_STUDIO_TRY(check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&factory))));
  ComPtr<IWICBitmapDecoder> decoder;
  LK_STUDIO_TRY(check(factory->CreateDecoderFromFilename(filename.c_str(), nullptr, GENERIC_READ,
                                                         WICDecodeMetadataCacheOnLoad, &decoder)));
  ComPtr<IWICBitmapFrameDecode> frame;
  LK_STUDIO_TRY(check(decoder->GetFrame(0, &frame)));
  ComPtr<IWICFormatConverter> convert;
  LK_STUDIO_TRY(check(factory->CreateFormatConverter(&convert)));
  LK_STUDIO_TRY(
      check(convert->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                nullptr, 0, WICBitmapPaletteTypeCustom)));
  UINT w, h;
  LK_STUDIO_TRY(check(convert->GetSize(&w, &h)));
  if (w < 1 || h < 1 || w > 8192 || h > 8192)
    return failed("图片尺寸超出限制");
  auto image = std::make_shared<StudioImage>();
  image->width = w;
  image->height = h;
  image->rgba.resize(size_t(w) * h * 4);
  LK_STUDIO_TRY(
      check(convert->CopyPixels(nullptr, w * 4, UINT(image->rgba.size()), image->rgba.data())));
  return Result<std::shared_ptr<StudioImage>>::success(std::move(image));
}

Result<std::shared_ptr<StudioImage>> text_image(const std::string& text,
                                                const std::array<float, 4>& color) {
  LK_STUDIO_ASSIGN(str, wide(text));
  for (float channel : color)
    if (!std::isfinite(channel) || channel < 0 || channel > 1)
      return failed("Invalid text color", ErrorCode::invalid_argument);
  const int w = 1280, h = 180;
  HDC dc = CreateCompatibleDC(nullptr);
  if (!dc)
    return failed("无法创建文字画布");
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void* pixels = nullptr;
  HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &pixels, nullptr, 0);
  if (!bitmap) {
    DeleteDC(dc);
    return failed("无法分配文字画布");
  }
  auto old = SelectObject(dc, bitmap);
  memset(pixels, 0, size_t(w) * h * 4);
  HFONT font = CreateFontW(-64, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                           DEFAULT_PITCH, L"Microsoft YaHei UI");
  auto oldFont = SelectObject(dc, font);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(255, 255, 255));
  RECT rect{4, 4, w - 4, h - 4};
  DrawTextW(dc, str.c_str(), int(str.size()), &rect, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
  GdiFlush();
  auto img = std::make_shared<StudioImage>();
  img->width = w;
  img->height = h;
  img->rgba.resize(size_t(w) * h * 4);
  for (size_t i = 0; i < img->rgba.size(); i += 4) {
    auto* p = static_cast<uint8_t*>(pixels) + i;
    img->rgba[i] = uint8_t(color[0] * 255);
    img->rgba[i + 1] = uint8_t(color[1] * 255);
    img->rgba[i + 2] = uint8_t(color[2] * 255);
    img->rgba[i + 3] = uint8_t(std::max({p[0], p[1], p[2]}) * color[3]);
  }
  SelectObject(dc, oldFont);
  DeleteObject(font);
  SelectObject(dc, old);
  DeleteObject(bitmap);
  DeleteDC(dc);
  return Result<std::shared_ptr<StudioImage>>::success(std::move(img));
}
}  // namespace livekit::sdk::studio_detail

namespace livekit::sdk {
Result<std::vector<StudioDevice>> studio_windows() {
  std::vector<StudioDevice> out;
  EnumWindows(
      [](HWND hwnd, LPARAM p) -> BOOL {
        if (!IsWindowVisible(hwnd) || GetWindowTextLengthW(hwnd) == 0)
          return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId())
          return TRUE;
        wchar_t title[1024]{};
        GetWindowTextW(hwnd, title, 1024);
        const int length = int(wcslen(title));
        const int size =
            WideCharToMultiByte(CP_UTF8, 0, title, length, nullptr, 0, nullptr, nullptr);
        std::string str(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, title, length, str.data(), size, nullptr, nullptr);
        reinterpret_cast<std::vector<StudioDevice>*>(p)->push_back(
            {std::to_string(reinterpret_cast<uintptr_t>(hwnd)),
             std::string(str.begin(), str.end())});
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&out));
  return Result<std::vector<StudioDevice>>::success(std::move(out));
}
}  // namespace livekit::sdk

namespace livekit::sdk {
Result<std::vector<StudioDevice>> studio_screens() {
  std::vector<StudioDevice> out;
  auto devices = screen_capture_devices();
  if (!devices)
    return Result<std::vector<StudioDevice>>::failure(devices.failure_record());
  for (auto& d : *devices)
    out.push_back({d.id, d.name});
  return Result<std::vector<StudioDevice>>::success(std::move(out));
}

Result<std::vector<StudioDevice>> studio_cameras() {
  std::vector<StudioDevice> out;
  int i = 0;
  auto devices = video_capture_devices();
  if (!devices)
    return Result<std::vector<StudioDevice>>::failure(devices.failure_record());
  for (auto& d : *devices)
    out.push_back({std::to_string(i++), d.name});
  return Result<std::vector<StudioDevice>>::success(std::move(out));
}
}  // namespace livekit::sdk
