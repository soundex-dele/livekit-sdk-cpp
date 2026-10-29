#include "media_nodes/capture/capture.hpp"
#include "media_nodes/capture/screen_selection.hpp"
#include "modules/video_capture/video_capture_factory.h"
#include "api/video/i420_buffer.h"
#include "rtc_base/time_utils.h"
#include "libyuv/convert.h"
#include <chrono>
#ifdef __APPLE__
#include "media_nodes/capture/screen_macos.hpp"
#endif
#include <thread>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
#ifdef _WIN32
struct DpiScope {
  using SetDpi = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
  SetDpi set = reinterpret_cast<SetDpi>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
  DPI_AWARENESS_CONTEXT previous = set ? set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : nullptr;

  DpiScope() {
    if (!previous)
      throw std::runtime_error("Per-monitor DPI context requires Windows 10 1703 or later");
  }

  ~DpiScope() {
    if (previous)
      set(previous);
  }
};

std::string utf8(const wchar_t* text) {
  int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (n < 1)
    throw std::runtime_error("Invalid display name");
  std::string result(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), n, nullptr, nullptr);
  result.pop_back();
  return result;
}
#endif
}  // namespace

namespace livekit::sdk {
CaptureSize screen_capture_size(int width, int height) {
  if (width < 2 || height < 2)
    throw std::invalid_argument("Invalid display dimensions");
  const double scale = std::min({1.0, (width >= height ? 1920.0 : 1080.0) / width,
                                 (width >= height ? 1080.0 : 1920.0) / height});
  return {std::max(2, static_cast<int>(width * scale) / 2 * 2),
          std::max(2, static_cast<int>(height * scale) / 2 * 2)};
}

std::vector<ScreenCaptureDevice> screen_capture_devices() {
  std::vector<ScreenCaptureDevice> result;
#ifdef __APPLE__
  return lk::mac_screen_devices();
#endif
#ifdef _WIN32
  DpiScope dpi;
  // Enumerate physical modes rather than DPI-virtualized window coordinates.
  for (DWORD index = 0;; ++index) {
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    if (!EnumDisplayDevicesW(nullptr, index, &device, 0))
      break;
    if (!(device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) ||
        (device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER))
      continue;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsExW(device.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0))
      throw std::runtime_error("Cannot read display mode");
    result.push_back(
        {utf8(device.DeviceName), utf8(device.DeviceString), mode.dmPosition.x, mode.dmPosition.y,
         static_cast<int>(mode.dmPelsWidth), static_cast<int>(mode.dmPelsHeight),
         (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0,
         static_cast<int>(mode.dmDisplayOrientation), static_cast<int>(mode.dmDisplayFrequency)});
  }
#endif
  return result;
}

std::vector<VideoCaptureDevice> video_capture_devices() {
  std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
      webrtc::VideoCaptureFactory::CreateDeviceInfo());
  if (!info)
    throw std::runtime_error("Cannot enumerate cameras");
  std::vector<VideoCaptureDevice> result;
  for (uint32_t i = 0; i < info->NumberOfDevices(); ++i) {
    char name[256]{}, id[1024]{};
    if (info->GetDeviceName(i, name, sizeof(name), id, sizeof(id)) != 0)
      throw std::runtime_error("Cannot read camera device information");
    result.push_back({name, id});
  }
  return result;
}
}  // namespace livekit::sdk

namespace lk {
namespace {
class CameraSink final : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  explicit CameraSink(const std::function<void(const webrtc::VideoFrame&)>& output)
      : output_(output) {}

  void OnFrame(const webrtc::VideoFrame& frame) override {
    try {
      output_(frame);
    } catch (...) {
      failed.store(true);
    }
  }

  std::atomic<bool> failed{false};

 private:
  const std::function<void(const webrtc::VideoFrame&)>& output_;
};
#ifdef _WIN32
struct ScreenSurface {
  HDC screen = GetDC(nullptr), memory = nullptr;
  HBITMAP bitmap = nullptr;
  HGDIOBJ old = nullptr;
  void* pixels = nullptr;

  void resize(int width, int height) {
    if (old) {
      SelectObject(memory, old);
      old = nullptr;
    }
    if (bitmap) {
      DeleteObject(bitmap);
      bitmap = nullptr;
    }
    if (!memory)
      memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!memory || !bitmap)
      throw std::runtime_error("Cannot allocate screen capture surface");
    old = SelectObject(memory, bitmap);
  }

  ~ScreenSurface() {
    if (old)
      SelectObject(memory, old);
    if (bitmap)
      DeleteObject(bitmap);
    if (memory)
      DeleteDC(memory);
    if (screen)
      ReleaseDC(nullptr, screen);
  }
};
#endif
}  // namespace

void capture_device(const livekit::sdk::MediaSpec& spec, std::atomic<bool>& stop,
                    const std::function<void(const webrtc::VideoFrame&)>& output) {
  if (spec.capture == livekit::sdk::CaptureKind::camera) {
    auto devices = livekit::sdk::video_capture_devices();
    if (spec.device_index < 0 || static_cast<size_t>(spec.device_index) >= devices.size())
      throw std::runtime_error("Camera index unavailable");
    auto device = webrtc::VideoCaptureFactory::Create(devices[spec.device_index].id.c_str());
    if (!device)
      throw std::runtime_error("Cannot open camera");
    std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
        webrtc::VideoCaptureFactory::CreateDeviceInfo());
    webrtc::VideoCaptureCapability requested, selected;
    requested.width = spec.width;
    requested.height = spec.height;
    requested.maxFPS = spec.fps;
    if (!info || info->GetBestMatchedCapability(devices[spec.device_index].id.c_str(), requested,
                                                selected) < 0)
      throw std::runtime_error("No compatible camera capture mode");
    CameraSink sink(output);
    device->RegisterCaptureDataCallback(&sink);
    const int started = device->StartCapture(selected);
    if (started == 0)
      while (!stop.load() && !sink.failed.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    device->StopCapture();
    device->DeRegisterCaptureDataCallback();
    if (started != 0 || sink.failed.load())
      throw std::runtime_error("Camera capture failed");
    return;
  }
#ifdef _WIN32
  DpiScope dpi;
  auto selected = select_screen(livekit::sdk::screen_capture_devices(), spec.screen_id);
  ScreenSurface surface;
  if (!surface.screen)
    throw std::runtime_error("Cannot access desktop");
  surface.resize(selected.width, selected.height);
  const auto period = std::chrono::microseconds(1000000 / spec.fps);
  auto next = std::chrono::steady_clock::now();
  while (!stop.load()) {
    auto displays = livekit::sdk::screen_capture_devices();
    if (!spec.screen_id.empty())
      validate_screen(selected, displays);
    else {
      // Preserve legacy callers that follow the primary display across mode changes.
      auto current = select_screen(displays, "");
      if (current.width != selected.width || current.height != selected.height)
        surface.resize(current.width, current.height);
      selected = std::move(current);
    }
    if (!BitBlt(surface.memory, 0, 0, selected.width, selected.height, surface.screen, selected.x,
                selected.y, SRCCOPY | CAPTUREBLT))
      throw std::runtime_error("Desktop capture failed");
    CURSORINFO cursor{};
    cursor.cbSize = sizeof(cursor);
    if (GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING) &&
        cursor.ptScreenPos.x >= selected.x && cursor.ptScreenPos.y >= selected.y &&
        cursor.ptScreenPos.x < selected.x + selected.width &&
        cursor.ptScreenPos.y < selected.y + selected.height) {
      ICONINFO icon{};
      if (GetIconInfo(cursor.hCursor, &icon)) {
        DrawIconEx(surface.memory,
                   cursor.ptScreenPos.x - selected.x - static_cast<int>(icon.xHotspot),
                   cursor.ptScreenPos.y - selected.y - static_cast<int>(icon.yHotspot),
                   cursor.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
        if (icon.hbmMask)
          DeleteObject(icon.hbmMask);
        if (icon.hbmColor)
          DeleteObject(icon.hbmColor);
      }
    }
    GdiFlush();
    auto native = webrtc::I420Buffer::Create(selected.width, selected.height);
    if (libyuv::ARGBToI420(static_cast<uint8_t*>(surface.pixels), selected.width * 4,
                           native->MutableDataY(), native->StrideY(), native->MutableDataU(),
                           native->StrideU(), native->MutableDataV(), native->StrideV(),
                           selected.width, selected.height) != 0)
      throw std::runtime_error("Screen conversion failed");
    auto buffer = webrtc::I420Buffer::Create(spec.width, spec.height);
    buffer->ScaleFrom(*native);
    output(webrtc::VideoFrame::Builder()
               .set_video_frame_buffer(buffer)
               .set_timestamp_us(webrtc::TimeMicros())
               .build());
    next += period;
    if (next < std::chrono::steady_clock::now() - period)
      next = std::chrono::steady_clock::now();
    std::this_thread::sleep_until(next);
  }
#elif defined(__APPLE__)
  capture_screen_macos(spec, stop, output);
#else
  throw std::runtime_error("Screen capture backend unavailable");
#endif
}
}  // namespace lk
