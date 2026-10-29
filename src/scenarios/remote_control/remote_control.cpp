#include <livekit/remote_control.hpp>
#include "scenarios/remote_control/text.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <stdexcept>

namespace livekit::sdk {
unsigned control_key_scan_code(const std::string& key) {
  static const std::map<std::string, unsigned> codes = {{"Escape", 1},
                                                        {"Digit1", 2},
                                                        {"Digit2", 3},
                                                        {"Digit3", 4},
                                                        {"Digit4", 5},
                                                        {"Digit5", 6},
                                                        {"Digit6", 7},
                                                        {"Digit7", 8},
                                                        {"Digit8", 9},
                                                        {"Digit9", 10},
                                                        {"Digit0", 11},
                                                        {"Minus", 12},
                                                        {"Equal", 13},
                                                        {"Backspace", 14},
                                                        {"Tab", 15},
                                                        {"KeyQ", 16},
                                                        {"KeyW", 17},
                                                        {"KeyE", 18},
                                                        {"KeyR", 19},
                                                        {"KeyT", 20},
                                                        {"KeyY", 21},
                                                        {"KeyU", 22},
                                                        {"KeyI", 23},
                                                        {"KeyO", 24},
                                                        {"KeyP", 25},
                                                        {"BracketLeft", 26},
                                                        {"BracketRight", 27},
                                                        {"Enter", 28},
                                                        {"ControlLeft", 29},
                                                        {"KeyA", 30},
                                                        {"KeyS", 31},
                                                        {"KeyD", 32},
                                                        {"KeyF", 33},
                                                        {"KeyG", 34},
                                                        {"KeyH", 35},
                                                        {"KeyJ", 36},
                                                        {"KeyK", 37},
                                                        {"KeyL", 38},
                                                        {"Semicolon", 39},
                                                        {"Quote", 40},
                                                        {"Backquote", 41},
                                                        {"ShiftLeft", 42},
                                                        {"Backslash", 43},
                                                        {"KeyZ", 44},
                                                        {"KeyX", 45},
                                                        {"KeyC", 46},
                                                        {"KeyV", 47},
                                                        {"KeyB", 48},
                                                        {"KeyN", 49},
                                                        {"KeyM", 50},
                                                        {"Comma", 51},
                                                        {"Period", 52},
                                                        {"Slash", 53},
                                                        {"ShiftRight", 54},
                                                        {"NumpadMultiply", 55},
                                                        {"AltLeft", 56},
                                                        {"Space", 57},
                                                        {"CapsLock", 58},
                                                        {"F1", 59},
                                                        {"F2", 60},
                                                        {"F3", 61},
                                                        {"F4", 62},
                                                        {"F5", 63},
                                                        {"F6", 64},
                                                        {"F7", 65},
                                                        {"F8", 66},
                                                        {"F9", 67},
                                                        {"F10", 68},
                                                        {"NumLock", 69},
                                                        {"ScrollLock", 70},
                                                        {"Numpad7", 71},
                                                        {"Numpad8", 72},
                                                        {"Numpad9", 73},
                                                        {"NumpadSubtract", 74},
                                                        {"Numpad4", 75},
                                                        {"Numpad5", 76},
                                                        {"Numpad6", 77},
                                                        {"NumpadAdd", 78},
                                                        {"Numpad1", 79},
                                                        {"Numpad2", 80},
                                                        {"Numpad3", 81},
                                                        {"Numpad0", 82},
                                                        {"NumpadDecimal", 83},
                                                        {"F11", 87},
                                                        {"F12", 88},
                                                        {"ControlRight", 0x11d},
                                                        {"AltRight", 0x138},
                                                        {"NumpadEnter", 0x11c},
                                                        {"NumpadDivide", 0x135},
                                                        {"Home", 0x147},
                                                        {"ArrowUp", 0x148},
                                                        {"PageUp", 0x149},
                                                        {"ArrowLeft", 0x14b},
                                                        {"ArrowRight", 0x14d},
                                                        {"End", 0x14f},
                                                        {"ArrowDown", 0x150},
                                                        {"PageDown", 0x151},
                                                        {"Insert", 0x152},
                                                        {"Delete", 0x153},
                                                        {"MetaLeft", 0x15b},
                                                        {"MetaRight", 0x15c},
                                                        {"ContextMenu", 0x15d}};
  auto it = codes.find(key);
  return it == codes.end() ? 0 : it->second;
}

struct RemoteControlSession::Impl {
  std::shared_ptr<ControlInputSink> sink;
  mutable std::mutex mutex;
  ControlGrant grant;
  ControlLayout layout;
  bool enabled = false, acknowledged = false, heartbeat_received = false;
  uint64_t sequence = 0;
  uint64_t text_id = 0;
  bool text_accepted = false, text_pending_release = false;
  std::string text_reason;
  ControlClock::time_point heartbeat;
  ControlClock::time_point initial_deadline;
  std::string stopped = "idle";
  std::set<std::string> keys;
  std::set<int> buttons;
  std::jthread watchdog;

  bool release() noexcept {
    bool ok = true;
    for (auto it = keys.begin(); it != keys.end();) {
      ControlInput e;
      e.type = "key";
      e.code = *it;
      if (sink->apply(e))
        it = keys.erase(it);
      else {
        ok = false;
        ++it;
      }
    }
    for (auto it = buttons.begin(); it != buttons.end();) {
      ControlInput e;
      e.type = "button";
      e.button = *it;
      if (sink->apply(e))
        it = buttons.erase(it);
      else {
        ok = false;
        ++it;
      }
    }
    if (text_pending_release) {
      ControlInput e;
      e.type = "text_release";
      if (sink->apply(e))
        text_pending_release = false;
      else
        ok = false;
    }
    return ok;
  }

  void stop(const std::string& why) noexcept {
    enabled = false;
    acknowledged = false;
    stopped = why;
    if (!release())
      stopped = "input_release_failed";
  }

  void tick(ControlClock::time_point now) {
    if (enabled) {
      if (now >= grant.deadline)
        stop("lease_expired");
      else if (!heartbeat_received ? now >= initial_deadline
                                   : now - heartbeat >= std::chrono::seconds(3))
        stop(heartbeat_received ? "heartbeat_timeout" : "control_connect_timeout");
    } else if (!keys.empty() || !buttons.empty() || text_pending_release)
      release();
  }
};

RemoteControlSession::RemoteControlSession(std::shared_ptr<ControlInputSink> sink, bool watchdog)
    : impl_(std::make_unique<Impl>()) {
  if (!sink)
    throw std::invalid_argument("input sink required");
  impl_->sink = std::move(sink);
  if (watchdog)
    impl_->watchdog = std::jthread([this](std::stop_token stop) {
      while (!stop.stop_requested()) {
        tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    });
}

RemoteControlSession::~RemoteControlSession() {
  impl_->watchdog.request_stop();
  if (impl_->watchdog.joinable())
    impl_->watchdog.join();
  stop();
}

void RemoteControlSession::authorize(ControlGrant grant, ControlLayout layout,
                                     ControlClock::time_point now) {
  std::lock_guard lock(impl_->mutex);
  impl_->stop("replaced");
  if (!impl_->keys.empty() || !impl_->buttons.empty() || impl_->text_pending_release)
    throw std::runtime_error("Previous input could not be released");
  if (grant.id.empty() || grant.controller_identity.empty() || !grant.epoch ||
      grant.deadline <= now || layout.width <= 0 || layout.height <= 0 || !layout.version)
    throw std::invalid_argument("invalid grant or layout");
  impl_->grant = std::move(grant);
  impl_->layout = layout;
  impl_->enabled = true;
  impl_->sequence = 0;
  impl_->heartbeat = now;
  impl_->stopped.clear();
  impl_->heartbeat_received = false;
  impl_->initial_deadline = now + std::chrono::seconds(10);
  impl_->text_id = 0;
  impl_->text_accepted = false;
  impl_->text_reason.clear();
}

bool RemoteControlSession::renew(const ControlGrant& grant, ControlClock::time_point now) {
  std::lock_guard lock(impl_->mutex);
  impl_->tick(now);
  if (!impl_->enabled || grant.id != impl_->grant.id || grant.epoch != impl_->grant.epoch ||
      grant.controller_identity != impl_->grant.controller_identity || grant.deadline <= now)
    return false;
  impl_->grant.deadline = grant.deadline;
  return true;
}

void RemoteControlSession::set_layout(ControlLayout layout) {
  std::lock_guard lock(impl_->mutex);
  if (layout.width <= 0 || layout.height <= 0 || layout.version <= impl_->layout.version)
    throw std::invalid_argument("layout version must increase");
  if (!impl_->release())
    impl_->stop("input_release_failed");
  impl_->layout = layout;
  impl_->acknowledged = false;
}

bool RemoteControlSession::receive(const std::vector<uint8_t>& bytes,
                                   const DataReceiveParams& params, ControlClock::time_point now) {
  std::lock_guard lock(impl_->mutex);
  auto& s = *impl_;
  s.tick(now);
  if (!s.enabled || !params.reliable || params.topic != remote_control_topic ||
      params.sender_identity != s.grant.controller_identity || bytes.size() > 4096)
    return false;
  try {
    auto v = Json::parse(bytes);
    if (v.at("v") != 1 || v.at("grant_id") != s.grant.id || v.at("epoch") != s.grant.epoch)
      return false;
    if (!v.at("seq").is_number_unsigned())
      return false;
    auto seq = v.at("seq").get<uint64_t>();
    if (seq <= s.sequence || seq > 9007199254740991ULL)
      return false;
    const auto type = v.at("type").get<std::string>();
    if (type == "heartbeat") {
      s.sequence = seq;
      s.heartbeat = now;
      s.heartbeat_received = true;
      return true;
    }
    if (type == "release" || type == "stop") {
      s.sequence = seq;
      if (type == "stop")
        s.stop("peer_stopped");
      else if (!s.release())
        s.stop("input_release_failed");
      return true;
    }
    // Transport setup has a bounded grace period, but no input or layout ack
    // may activate control before the authenticated peer's first heartbeat.
    if (!s.heartbeat_received)
      return false;
    if (v.at("layout_version") != s.layout.version)
      return false;
    if (type == "layout_ack") {
      s.sequence = seq;
      s.acknowledged = true;
      return true;
    }
    if (!s.acknowledged)
      return false;
    ControlInput event;
    event.type = type;
    if (type == "text") {
      const auto& id = v.at("text_id");
      if (!id.is_number_unsigned() || id == 0 || id > 9007199254740991ULL ||
          id.get<uint64_t>() <= s.text_id)
        return false;
      s.sequence = seq;
      s.text_id = id.get<uint64_t>();
      s.text_accepted = false;
      s.text_reason = "invalid_text";
      if (!v.contains("text") || !v.at("text").is_string())
        return false;
      event.text = v.at("text").get<std::string>();
      if (!detail::control_text_utf16(event.text))
        return false;
      if (!s.sink->supports_text()) {
        s.text_reason = "unsupported";
        return false;
      }
      // Text must not inherit held Ctrl/Alt/Shift or mouse buttons.
      if (!s.release()) {
        s.text_reason = "input_failed";
        s.stop("input_release_failed");
        return false;
      }
      s.text_pending_release = true;
      if (!s.sink->apply(event)) {
        s.text_reason = "input_failed";
        s.stop("input_failed");
        return false;
      }
      s.text_pending_release = false;
      s.text_accepted = true;
      s.text_reason.clear();
      return true;
    }
    if (type == "move" || type == "button" || type == "wheel") {
      double x = v.at("x").get<double>(), y = v.at("y").get<double>();
      if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 || y < 0 || y > 1)
        return false;
      event.x = s.layout.x + static_cast<int>(std::lround(x * (s.layout.width - 1)));
      event.y = s.layout.y + static_cast<int>(std::lround(y * (s.layout.height - 1)));
      event.positioned = true;
    }
    if (type == "button") {
      const auto& button = v.at("button");
      if (!button.is_number_integer() || button < 0 || button > 2)
        return false;
      event.button = button.get<int>();
      event.down = v.at("down").get<bool>();
    } else if (type == "key") {
      event.code = v.at("code").get<std::string>();
      event.down = v.at("down").get<bool>();
      if (!control_key_scan_code(event.code))
        return false;
    } else if (type == "wheel") {
      const auto& delta = v.at("delta");
      if (!delta.is_number_integer() || delta < -1200 || delta > 1200)
        return false;
      event.delta = delta.get<int>();
    } else if (type != "move")
      return false;
    s.sequence = seq;
    // Track downs before injection: partial native failures still require releases.
    if (type == "key" && event.down)
      s.keys.insert(event.code);
    if (type == "button" && event.down)
      s.buttons.insert(event.button);
    if (!s.sink->apply(event)) {
      s.stop("input_failed");
      return false;
    }
    if (type == "key" && !event.down)
      s.keys.erase(event.code);
    if (type == "button" && !event.down)
      s.buttons.erase(event.button);
    return true;
  } catch (const Json::exception&) {
    return false;
  }
}

void RemoteControlSession::tick(ControlClock::time_point now) {
  std::lock_guard lock(impl_->mutex);
  impl_->tick(now);
}

void RemoteControlSession::stop(const std::string& reason) noexcept {
  std::lock_guard lock(impl_->mutex);
  impl_->stop(reason);
}

bool RemoteControlSession::active() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->enabled;
}

std::string RemoteControlSession::reason() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->stopped;
}

Json RemoteControlSession::status() const {
  std::lock_guard lock(impl_->mutex);
  auto& s = *impl_;
  return Json{{"v", 1},
              {"type", "layout"},
              {"grant_id", s.grant.id},
              {"epoch", s.grant.epoch},
              {"layout_version", s.layout.version},
              {"width", s.layout.width},
              {"height", s.layout.height},
              {"active", s.enabled},
              {"text_input", s.sink->supports_text()},
              {"text_result",
               {{"id", s.text_id}, {"accepted", s.text_accepted}, {"reason", s.text_reason}}}};
}
}  // namespace livekit::sdk
