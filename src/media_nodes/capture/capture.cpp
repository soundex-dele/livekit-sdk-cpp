#include "media_nodes/capture/capture.hpp"
#include "modules/video_capture/video_capture_factory.h"
#include "api/video/i420_buffer.h"
#include "rtc_base/time_utils.h"
#include "libyuv/convert.h"
#include <chrono>
#include <thread>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace livekit::sdk {
std::vector<VideoCaptureDevice> video_capture_devices() {
  std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(webrtc::VideoCaptureFactory::CreateDeviceInfo());
  if (!info) throw std::runtime_error("Cannot enumerate cameras");
  std::vector<VideoCaptureDevice> result;
  for (uint32_t i = 0; i < info->NumberOfDevices(); ++i) {
    char name[256]{}, id[1024]{};
    if (info->GetDeviceName(i, name, sizeof(name), id, sizeof(id)) != 0)
      throw std::runtime_error("Cannot read camera device information");
    result.push_back({name, id});
  }
  return result;
}
}
namespace lk {
namespace {
class CameraSink final : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  explicit CameraSink(const std::function<void(const webrtc::VideoFrame&)>& output) : output_(output) {}
  void OnFrame(const webrtc::VideoFrame& frame) override {
    try { output_(frame); } catch (...) { failed.store(true); }
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
  ~ScreenSurface() {
    if (old) SelectObject(memory, old);
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    if (screen) ReleaseDC(nullptr, screen);
  }
};
#endif
}
void capture_device(const livekit::sdk::MediaSpec& spec, std::atomic<bool>& stop,
                    const std::function<void(const webrtc::VideoFrame&)>& output) {
  if (spec.capture == livekit::sdk::CaptureKind::camera) {
    auto devices = livekit::sdk::video_capture_devices();
    if (spec.device_index < 0 || static_cast<size_t>(spec.device_index) >= devices.size())
      throw std::runtime_error("Camera index unavailable");
    auto device = webrtc::VideoCaptureFactory::Create(devices[spec.device_index].id.c_str());
    if (!device) throw std::runtime_error("Cannot open camera");
    std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(webrtc::VideoCaptureFactory::CreateDeviceInfo());
    webrtc::VideoCaptureCapability requested, selected;
    requested.width = spec.width; requested.height = spec.height; requested.maxFPS = spec.fps;
    if (!info || info->GetBestMatchedCapability(devices[spec.device_index].id.c_str(), requested, selected) < 0)
      throw std::runtime_error("No compatible camera capture mode");
    CameraSink sink(output);
    device->RegisterCaptureDataCallback(&sink);
    const int started = device->StartCapture(selected);
    if (started == 0) while (!stop.load() && !sink.failed.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    device->StopCapture(); device->DeRegisterCaptureDataCallback();
    if (started != 0 || sink.failed.load()) throw std::runtime_error("Camera capture failed");
    return;
  }
#ifdef _WIN32
  ScreenSurface surface;
  if (!surface.screen) throw std::runtime_error("Cannot access desktop");
  surface.memory = CreateCompatibleDC(surface.screen);
  BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = spec.width; info.bmiHeader.biHeight = -spec.height;
  info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
  surface.bitmap = CreateDIBSection(surface.screen, &info, DIB_RGB_COLORS, &surface.pixels, nullptr, 0);
  if (!surface.memory || !surface.bitmap) throw std::runtime_error("Cannot allocate screen capture surface");
  surface.old = SelectObject(surface.memory, surface.bitmap);
  SetStretchBltMode(surface.memory, HALFTONE);
  const auto period = std::chrono::microseconds(1000000 / spec.fps);
  auto next = std::chrono::steady_clock::now();
  while (!stop.load()) {
    const int width = GetSystemMetrics(SM_CXSCREEN), height = GetSystemMetrics(SM_CYSCREEN);
    if (!StretchBlt(surface.memory, 0, 0, spec.width, spec.height, surface.screen,
                    0, 0, width, height, SRCCOPY | CAPTUREBLT)) throw std::runtime_error("Desktop capture failed");
    GdiFlush();
    auto buffer = webrtc::I420Buffer::Create(spec.width, spec.height);
    if (libyuv::ARGBToI420(static_cast<uint8_t*>(surface.pixels), spec.width * 4,
        buffer->MutableDataY(), buffer->StrideY(), buffer->MutableDataU(), buffer->StrideU(),
        buffer->MutableDataV(), buffer->StrideV(), spec.width, spec.height) != 0)
      throw std::runtime_error("Screen conversion failed");
    output(webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer).set_timestamp_us(webrtc::TimeMicros()).build());
    next += period; if (next < std::chrono::steady_clock::now() - period) next = std::chrono::steady_clock::now();
    std::this_thread::sleep_until(next);
  }
#else
  throw std::runtime_error("Screen capture backend is currently Windows-only");
#endif
}
}
