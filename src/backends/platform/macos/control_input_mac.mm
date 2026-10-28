#include "control_input.hpp"
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <array>
#include <chrono>
#include <cmath>
namespace livekit::sdk {
ControlPermissions control_permissions() {
  @autoreleasepool {
    bool interactive = false;
    if (auto d = CGSessionCopyCurrentDictionary()) {
      auto dict = (__bridge NSDictionary *)d;
      interactive =
          [dict[(__bridge NSString *)kCGSessionOnConsoleKey] boolValue] &&
          [dict[(__bridge NSString *)kCGSessionLoginDoneKey] boolValue] &&
          ![dict[@"CGSSessionScreenIsLocked"] boolValue];
      CFRelease(d);
    }
    return {bool(CGPreflightScreenCaptureAccess()), bool(AXIsProcessTrusted()),
            interactive};
  }
}
ControlLayout primary_control_layout() {
  for (const auto &screen : screen_capture_devices())
    if (screen.primary)
      return {1, 0, 0, screen.width, screen.height};
  throw std::runtime_error("主屏幕不可用");
}
namespace {
class NativeMacInput : public detail::MacInputApi {
  std::array<int, 3> clicks_{};
  std::array<CGPoint, 3> click_position_{};
  std::array<std::chrono::steady_clock::time_point, 3> click_time_{};

public:
  bool available() noexcept override {
    auto p = control_permissions();
    return p.input && p.interactive;
  }
  detail::MacDisplayGeometry display() noexcept override {
    try {
      auto layout = primary_control_layout();
      auto b = CGDisplayBounds(CGMainDisplayID());
      return {layout.width, layout.height, b.origin.x,
              b.origin.y,   b.size.width,  b.size.height};
    } catch (...) {
      return {};
    }
  }
  bool send(const detail::MacInputEvent &in) noexcept override {
    CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStatePrivate);
    if (!source)
      return false;
    CGEventRef e = nullptr;
    CGPoint p = CGPointMake(in.x, in.y);
    CGMouseButton button = in.button == 1   ? kCGMouseButtonCenter
                           : in.button == 2 ? kCGMouseButtonRight
                                            : kCGMouseButtonLeft;
    if (in.type == "key" || in.type == "text") {
      auto key = in.key;
      if (in.type == "text" && in.text == u"\r")
        key = 36;
      else if (in.type == "text" && in.text == u"\t")
        key = 48;
      e = CGEventCreateKeyboardEvent(source, key, in.down);
      if (e && in.type == "text" && in.text != u"\r" && in.text != u"\t")
        CGEventKeyboardSetUnicodeString(
            e, in.text.size(),
            reinterpret_cast<const UniChar *>(in.text.data()));
    } else if (in.type == "wheel") {
      auto move = CGEventCreateMouseEvent(source, kCGEventMouseMoved, p,
                                          kCGMouseButtonLeft);
      if (!move) {
        CFRelease(source);
        return false;
      }
      CGEventPost(kCGHIDEventTap, move);
      CFRelease(move);
      e = CGEventCreateScrollWheelEvent(source, kCGScrollEventUnitLine, 1,
                                        in.delta / 120);
    } else {
      CGEventType type = kCGEventMouseMoved;
      if (in.type == "button_release") {
        auto cursor = CGEventCreate(nullptr);
        if (!cursor) {
          CFRelease(source);
          return false;
        }
        p = CGEventGetLocation(cursor);
        CFRelease(cursor);
      }
      if (in.type == "button" || in.type == "button_release")
        type = button == kCGMouseButtonLeft
                   ? (in.down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp)
               : button == kCGMouseButtonRight
                   ? (in.down ? kCGEventRightMouseDown : kCGEventRightMouseUp)
                   : (in.down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp);
      if (in.type == "drag")
        type = button == kCGMouseButtonLeft    ? kCGEventLeftMouseDragged
               : button == kCGMouseButtonRight ? kCGEventRightMouseDragged
                                               : kCGEventOtherMouseDragged;
      e = CGEventCreateMouseEvent(source, type, p, button);
      if (e && (in.type == "button" || in.type == "button_release")) {
        const int index = int(button);
        if (in.down) {
          auto now = std::chrono::steady_clock::now();
          auto previous = click_position_[index];
          bool close = std::hypot(p.x - previous.x, p.y - previous.y) <= 4;
          bool quick =
              std::chrono::duration<double>(now - click_time_[index]).count() <
              [NSEvent doubleClickInterval];
          clicks_[index] = close && quick ? std::min(clicks_[index] + 1, 3) : 1;
          click_position_[index] = p;
          click_time_[index] = now;
        }
        CGEventSetIntegerValueField(e, kCGMouseEventClickState,
                                    std::max(1, clicks_[index]));
      }
    }
    CFRelease(source);
    if (!e)
      return false;
    CGEventSetFlags(e, in.flags);
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
    return true;
  }
};
} // namespace
std::shared_ptr<ControlInputSink> make_platform_control_input() {
  return detail::mac_input_with_api(std::make_shared<NativeMacInput>());
}
} // namespace livekit::sdk
