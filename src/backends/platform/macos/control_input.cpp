#include "control_input.hpp"
#include "scenarios/remote_control/text.hpp"
#include <map>
#include <stdexcept>

namespace livekit::sdk::detail {
std::optional<int> mac_key_code(const std::string& code) {
  static const std::map<std::string, int> keys = {{"KeyA", 0},
                                                  {"KeyS", 1},
                                                  {"KeyD", 2},
                                                  {"KeyF", 3},
                                                  {"KeyH", 4},
                                                  {"KeyG", 5},
                                                  {"KeyZ", 6},
                                                  {"KeyX", 7},
                                                  {"KeyC", 8},
                                                  {"KeyV", 9},
                                                  {"KeyB", 11},
                                                  {"KeyQ", 12},
                                                  {"KeyW", 13},
                                                  {"KeyE", 14},
                                                  {"KeyR", 15},
                                                  {"KeyY", 16},
                                                  {"KeyT", 17},
                                                  {"Digit1", 18},
                                                  {"Digit2", 19},
                                                  {"Digit3", 20},
                                                  {"Digit4", 21},
                                                  {"Digit6", 22},
                                                  {"Digit5", 23},
                                                  {"Equal", 24},
                                                  {"Digit9", 25},
                                                  {"Digit7", 26},
                                                  {"Minus", 27},
                                                  {"Digit8", 28},
                                                  {"Digit0", 29},
                                                  {"BracketRight", 30},
                                                  {"KeyO", 31},
                                                  {"KeyU", 32},
                                                  {"BracketLeft", 33},
                                                  {"KeyI", 34},
                                                  {"KeyP", 35},
                                                  {"Enter", 36},
                                                  {"KeyL", 37},
                                                  {"KeyJ", 38},
                                                  {"Quote", 39},
                                                  {"KeyK", 40},
                                                  {"Semicolon", 41},
                                                  {"Backslash", 42},
                                                  {"Comma", 43},
                                                  {"Slash", 44},
                                                  {"KeyN", 45},
                                                  {"KeyM", 46},
                                                  {"Period", 47},
                                                  {"Tab", 48},
                                                  {"Space", 49},
                                                  {"Backquote", 50},
                                                  {"Backspace", 51},
                                                  {"Escape", 53},
                                                  {"MetaRight", 54},
                                                  {"MetaLeft", 55},
                                                  {"ShiftLeft", 56},
                                                  {"CapsLock", 57},
                                                  {"AltLeft", 58},
                                                  {"ControlLeft", 59},
                                                  {"ShiftRight", 60},
                                                  {"AltRight", 61},
                                                  {"ControlRight", 62},
                                                  {"NumpadDecimal", 65},
                                                  {"NumpadMultiply", 67},
                                                  {"NumpadAdd", 69},
                                                  {"NumLock", 71},
                                                  {"NumpadDivide", 75},
                                                  {"NumpadEnter", 76},
                                                  {"NumpadSubtract", 78},
                                                  {"Numpad0", 82},
                                                  {"Numpad1", 83},
                                                  {"Numpad2", 84},
                                                  {"Numpad3", 85},
                                                  {"Numpad4", 86},
                                                  {"Numpad5", 87},
                                                  {"Numpad6", 88},
                                                  {"Numpad7", 89},
                                                  {"Numpad8", 91},
                                                  {"Numpad9", 92},
                                                  {"F1", 122},
                                                  {"F2", 120},
                                                  {"F3", 99},
                                                  {"F4", 118},
                                                  {"F5", 96},
                                                  {"F6", 97},
                                                  {"F7", 98},
                                                  {"F8", 100},
                                                  {"F9", 101},
                                                  {"F10", 109},
                                                  {"F11", 103},
                                                  {"F12", 111},
                                                  {"Home", 115},
                                                  {"PageUp", 116},
                                                  {"Delete", 117},
                                                  {"End", 119},
                                                  {"PageDown", 121},
                                                  {"ArrowLeft", 123},
                                                  {"ArrowRight", 124},
                                                  {"ArrowDown", 125},
                                                  {"ArrowUp", 126},
                                                  {"Insert", 114}};
  auto it = keys.find(code);
  if (it == keys.end())
    return {};
  return it->second;
}

class MacInput final : public ControlInputSink {
  std::shared_ptr<MacInputApi> api_;
  std::set<std::string> held_;
  std::set<int> buttons_;
  std::u16string pending_text_;

  uint64_t flags() const {
    uint64_t f = 0;
    for (const auto& k : held_) {
      if (k.starts_with("Shift"))
        f |= 1ULL << 17;
      else if (k.starts_with("Control"))
        f |= 1ULL << 18;
      else if (k.starts_with("Alt"))
        f |= 1ULL << 19;
      else if (k.starts_with("Meta"))
        f |= 1ULL << 20;
    }
    return f;
  }

 public:
  explicit MacInput(std::shared_ptr<MacInputApi> a) : api_(std::move(a)) {}

  bool supports_text() const noexcept override {
    return true;
  }

  bool apply(const ControlInput& in) noexcept override {
    try {
      if (in.type == "text_release" && pending_text_.empty())
        return true;
      if (!api_->available())
        return false;
      MacInputEvent e;
      e.type = in.type;
      e.button = in.button;
      e.down = in.down;
      e.flags = flags();
      if (in.type == "text_release") {
        e.type = "text";
        e.text = pending_text_;
        if (!api_->send(e))
          return false;
        pending_text_.clear();
        return true;
      }
      if (in.type == "text") {
        auto text = control_text_utf16(in.text);
        if (!text || !pending_text_.empty())
          return false;
        // Send one Unicode scalar at a time, preserving surrogate pairs. Enter
        // and Tab use physical events because controls need not interpret
        // Unicode control chars.
        char16_t previous = 0;
        for (size_t i = 0; i < text->size(); ++i) {
          char16_t c = (*text)[i];
          if (c == '\n' && previous == '\r') {
            previous = c;
            continue;
          }
          previous = c;
          e = {};
          e.type = "text";
          e.down = true;
          e.text.assign(1, c == '\n' ? u'\r' : c);
          if (c >= 0xd800 && c <= 0xdbff)
            e.text += (*text)[++i];
          pending_text_ = e.text;
          if (!api_->send(e))
            return false;
          e.down = false;
          if (!api_->send(e))
            return false;
          pending_text_.clear();
        }
        return true;
      }
      if (in.type == "key") {
        auto key = mac_key_code(in.code);
        if (!key)
          return false;
        e.key = *key;
        auto next = held_;
        if (in.down)
          held_.insert(in.code);
        else
          held_.erase(in.code);
        e.flags = flags();
        if (!api_->send(e)) {
          held_ = std::move(next);
          return false;
        }
        return true;
      }
      const auto g = api_->display();
      if (g.width < 2 || g.height < 2 || g.logical_width <= 0 || g.logical_height <= 0)
        return false;
      if (in.positioned) {
        e.x = g.x + double(in.x) * g.logical_width / g.width;
        e.y = g.y + double(in.y) * g.logical_height / g.height;
      } else {
        e.type = "button_release";
      }
      if (in.type == "move" && !buttons_.empty()) {
        e.type = "drag";
        e.button = *buttons_.begin();
      }
      if (in.type == "button" && (in.button < 0 || in.button > 2))
        return false;
      e.delta = in.delta;
      if (!api_->send(e))
        return false;
      if (in.type == "button") {
        if (in.down)
          buttons_.insert(in.button);
        else
          buttons_.erase(in.button);
      }
      return true;
    } catch (...) {
      return false;
    }
  }
};

std::shared_ptr<ControlInputSink> mac_input_with_api(std::shared_ptr<MacInputApi> a) {
  if (!a)
    throw std::invalid_argument("Mac input API required");
  return std::make_shared<MacInput>(std::move(a));
}
}  // namespace livekit::sdk::detail
