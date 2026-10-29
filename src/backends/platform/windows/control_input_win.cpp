#include <livekit/remote_control.hpp>
#include "backends/platform/windows/control_input_win.hpp"
#include "scenarios/remote_control/text.hpp"
#include <array>
#include <stdexcept>
#include <algorithm>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace livekit::sdk {
#ifdef _WIN32
namespace {
HANDLE set_dpi(HANDLE value) {
  using Fn = HANDLE(WINAPI*)(HANDLE);
  static auto fn = reinterpret_cast<Fn>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
  return fn ? fn(value) : nullptr;
}

class SystemInputApi final : public detail::WindowsInputApi {
 public:
  bool desktop_available() noexcept override {
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desktop)
      return false;
    wchar_t name[256]{};
    DWORD size = 0;
    bool normal = GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &size) &&
                  _wcsicmp(name, L"Default") == 0;
    CloseDesktop(desktop);
    return normal;
  }

  ControlLayout virtual_layout() noexcept override {
    const auto old = set_dpi(reinterpret_cast<HANDLE>(-4));
    ControlLayout result{1, GetSystemMetrics(SM_XVIRTUALSCREEN),
                         GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN),
                         GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    if (old)
      set_dpi(old);
    return result;
  }

  unsigned send(std::span<const INPUT> inputs) noexcept override {
    return SendInput(static_cast<UINT>(inputs.size()), const_cast<INPUT*>(inputs.data()),
                     sizeof(INPUT));
  }
};

class WindowsInput final : public ControlInputSink {
  std::shared_ptr<detail::WindowsInputApi> api_;
  std::optional<WORD> pending_unicode_up_;

 public:
  explicit WindowsInput(std::shared_ptr<detail::WindowsInputApi> api) : api_(std::move(api)) {}

  bool supports_text() const noexcept override {
    return true;
  }

  bool apply(const ControlInput& event) noexcept override {
    if (event.type == "text_release" && !pending_unicode_up_)
      return true;
    INPUT inputs[2]{};
    UINT count = 0;
    // Refuse injection into a different / secure input desktop.
    if (!api_->desktop_available())
      return false;
    if (event.type == "text_release") {
      INPUT up{};
      up.type = INPUT_KEYBOARD;
      up.ki.wScan = *pending_unicode_up_;
      up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
      if (api_->send(std::span<const INPUT>(&up, 1)) != 1)
        return false;
      pending_unicode_up_.reset();
      return true;
    }
    if (event.type == "text") {
      if (pending_unicode_up_)
        return false;
      try {
        const auto units = detail::control_text_utf16(event.text);
        if (!units)
          return false;
        std::array<INPUT, 1024> text_inputs{};
        size_t used = 0;
        char16_t previous = 0;
        for (const auto unit : *units) {
          if (unit == '\n' && previous == '\r') {
            previous = unit;
            continue;
          }
          const auto value = unit == '\n' ? '\r' : unit;
          previous = unit;
          auto& down = text_inputs[used++];
          down.type = INPUT_KEYBOARD;
          down.ki.wScan = static_cast<WORD>(value);
          down.ki.dwFlags = KEYEVENTF_UNICODE;
          auto& up = text_inputs[used++];
          up = down;
          up.ki.dwFlags |= KEYEVENTF_KEYUP;
        }
        const auto sent = api_->send(std::span<const INPUT>(text_inputs.data(), used));
        if (sent < used && (sent % 2))
          pending_unicode_up_ = text_inputs[sent - 1].ki.wScan;
        return sent == used;
      } catch (...) {
        return false;
      }
    }
    if (event.type == "key") {
      auto scan = control_key_scan_code(event.code);
      if (!scan)
        return false;
      auto& input = inputs[count++];
      input.type = INPUT_KEYBOARD;
      input.ki.wScan = static_cast<WORD>(scan & 255);
      input.ki.dwFlags = KEYEVENTF_SCANCODE | (scan & 256 ? KEYEVENTF_EXTENDEDKEY : 0) |
                         (event.down ? 0 : KEYEVENTF_KEYUP);
    } else {
      // Releases from stop() must not move the pointer to an unrelated position.
      if (event.positioned) {
        auto layout = api_->virtual_layout();
        int x = layout.x, y = layout.y, w = layout.width, h = layout.height;
        if (w < 2 || h < 2)
          return false;
        auto& input = inputs[count++];
        input.type = INPUT_MOUSE;
        input.mi.dx = static_cast<LONG>((static_cast<int64_t>(event.x) - x) * 65535 / (w - 1));
        input.mi.dy = static_cast<LONG>((static_cast<int64_t>(event.y) - y) * 65535 / (h - 1));
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
      }
      if (event.type == "button") {
        if (event.button < 0 || event.button > 2)
          return false;
        static const DWORD down[] = {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_MIDDLEDOWN,
                                     MOUSEEVENTF_RIGHTDOWN},
                           up[] = {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_RIGHTUP};
        auto& input = inputs[count++];
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = event.down ? down[event.button] : up[event.button];
      } else if (event.type == "wheel") {
        auto& input = inputs[count++];
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = static_cast<DWORD>(event.delta);
      } else if (event.type != "move")
        return false;
    }
    return api_->send(std::span<const INPUT>(inputs, count)) == count;
  }
};
}  // namespace

std::shared_ptr<ControlInputSink> detail::windows_input_with_api(
    std::shared_ptr<detail::WindowsInputApi> api) {
  if (!api)
    throw std::invalid_argument("Windows input API required");
  return std::make_shared<WindowsInput>(std::move(api));
}

std::shared_ptr<ControlInputSink> make_windows_control_input() {
  return detail::windows_input_with_api(std::make_shared<SystemInputApi>());
}

std::shared_ptr<ControlInputSink> make_platform_control_input() {
  return make_windows_control_input();
}

ControlPermissions control_permissions() {
  SystemInputApi api;
  return {true, true, api.desktop_available()};
}

ControlLayout primary_control_layout() {
  const auto old = set_dpi(reinterpret_cast<HANDLE>(-4));
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  POINT point{};
  bool ok = GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY), &info) != 0;
  if (old)
    set_dpi(old);
  if (!ok)
    throw std::runtime_error("Primary screen unavailable");
  return {1, info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right - info.rcMonitor.left,
          info.rcMonitor.bottom - info.rcMonitor.top};
}
#else
std::shared_ptr<ControlInputSink> make_windows_control_input() {
  throw std::runtime_error("Windows input unavailable");
}
#ifndef __APPLE__
std::shared_ptr<ControlInputSink> make_platform_control_input() {
  return make_windows_control_input();
}

ControlPermissions control_permissions() {
  return {};
}

ControlLayout primary_control_layout() {
  throw std::runtime_error("Screen unavailable");
}
#endif
#endif
}  // namespace livekit::sdk
