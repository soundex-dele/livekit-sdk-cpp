#pragma once
#include <livekit/remote_control.hpp>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <span>
namespace livekit::sdk::detail {
// Private native boundary: tests substitute this interface and never touch a desktop.
class WindowsInputApi {
 public:
  virtual ~WindowsInputApi() = default;
  virtual bool desktop_available() noexcept = 0;
  virtual ControlLayout virtual_layout() noexcept = 0;
  virtual unsigned send(std::span<const INPUT>) noexcept = 0;
};
std::shared_ptr<ControlInputSink> windows_input_with_api(std::shared_ptr<WindowsInputApi>);
}
#endif
