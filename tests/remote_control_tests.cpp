#include <livekit/remote_control.hpp>
#include "backends/platform/windows/control_input_win.hpp"
#include "scenarios/remote_control/text.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace livekit::sdk;
using namespace std::chrono_literals;

struct Recorder : ControlInputSink {
  std::vector<ControlInput> events;
  bool fail = false, text_enabled = true;

  bool supports_text() const noexcept override {
    return text_enabled;
  }

  bool apply(const ControlInput& e) noexcept override {
    events.push_back(e);
    return !fail;
  }
};

void check(bool value, const char* text) {
  if (!value)
    throw std::runtime_error(text);
}

void handshake_tests() {
  auto sink = std::make_shared<Recorder>();
  RemoteControlSession session(sink, false);
  auto began = ControlClock::now(), now = began;
  ControlGrant grant{"g", "controller", 1, began + 60s};
  uint64_t seq = 0;
  auto send = [&](Json v) {
    v["v"] = 1;
    v["grant_id"] = "g";
    v["epoch"] = 1;
    v["seq"] = ++seq;
    v["layout_version"] = 1;
    auto bytes = v.dump();
    return session.receive({bytes.begin(), bytes.end()}, {"controller", remote_control_topic, true},
                           now);
  };
  session.authorize(grant, {1, 0, 0, 100, 100}, now);
  now += 5s;
  session.tick(now);
  check(session.active(), "transport setup may take more than three seconds");
  check(!send({{"type", "layout_ack"}}) &&
            !send({{"type", "key"}, {"code", "KeyA"}, {"down", true}}) && sink->events.empty(),
        "setup grace cannot enable input");
  check(send({{"type", "heartbeat"}}) && send({{"type", "layout_ack"}}),
        "delayed first heartbeat activates acknowledged layout");
  check(send({{"type", "key"}, {"code", "ShiftLeft"}, {"down", true}}),
        "active key after handshake");
  session.tick(now + 3s);
  check(!session.active() && session.reason() == "heartbeat_timeout" && !sink->events.back().down,
        "active connection still releases at three seconds");
  now = began;
  session.authorize(grant, {1, 0, 0, 100, 100}, now);
  seq = 0;
  auto renewal = grant;
  renewal.deadline = began + 120s;
  check(session.renew(renewal, began + 9s), "business may renew during setup");
  session.tick(began + 10s);
  check(!session.active() && session.reason() == "control_connect_timeout",
        "renewals cannot extend initial handshake deadline");
  now = began + 10s;
  check(!send({{"type", "heartbeat"}}), "late heartbeat cannot resurrect expired setup");
  grant.deadline = began + 2s;
  session.authorize(grant, {1, 0, 0, 100, 100}, began);
  session.tick(began + 2s);
  check(!session.active() && session.reason() == "lease_expired",
        "business lease bounds setup grace");
}

void text_tests() {
  const std::string unicode = "\xe4\xbd\xa0\xe5\xa5\xbd \xf0\x9f\x98\x80\r\n\t";
  check(detail::control_text_utf16(unicode) == u"\u4f60\u597d \U0001f600\r\n\t",
        "UTF-8 preserves CJK, emoji and whitespace");
  for (const auto& invalid : std::vector<std::string>{
           "", std::string(257, 'a'), std::string(1, '\0'), "\x1b", "\xc2\x85", "\xc0\xaf",
           "\xe0\x80\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf0\x9f", "\x80", "\xff"})
    check(!detail::control_text_utf16(invalid), "reject malformed, forbidden or oversized text");
  std::string limit;
  for (int i = 0; i < 256; ++i)
    limit += "\xf0\x9f\x98\x80";
  check(detail::control_text_utf16(limit)->size() == 512, "256 supplementary characters accepted");
  check(!detail::control_text_utf16(limit + "x"), "257 scalars rejected");
  auto sink = std::make_shared<Recorder>();
  RemoteControlSession session(sink, false);
  auto now = ControlClock::now();
  session.authorize({"g", "controller", 1, now + 10s}, {1, 0, 0, 100, 100}, now);
  uint64_t sequence = 0;
  DataReceiveParams params{"controller", remote_control_topic, true};
  auto send = [&](Json v) {
    v["v"] = 1;
    v["grant_id"] = "g";
    v["epoch"] = 1;
    if (!v.contains("seq"))
      v["seq"] = ++sequence;
    if (!v.contains("layout_version"))
      v["layout_version"] = 1;
    auto bytes = v.dump();
    return session.receive({bytes.begin(), bytes.end()}, params, now);
  };
  auto message = Json{{"type", "text"}, {"text_id", 1u}, {"text", unicode}};
  check(send({{"type", "heartbeat"}}), "initial text heartbeat");
  check(!send(message), "text needs layout ack");
  check(sink->events.empty(), "no unacknowledged text");
  check(send({{"type", "layout_ack"}}), "text ack");
  params.sender_identity = "other";
  check(!send(message), "text identity gate");
  params.sender_identity = "controller";
  params.reliable = false;
  check(!send(message), "text reliability gate");
  params.reliable = true;
  auto stale = message;
  stale["layout_version"] = 2;
  check(!send(stale), "text layout gate");
  stale = message;
  stale["seq"] = 1u;
  check(!send(stale), "text sequence gate");
  check(sink->events.empty(), "rejected text never reaches sink");
  check(send({{"type", "key"}, {"code", "ControlLeft"}, {"down", true}}), "hold modifier");
  check(send({{"type", "button"}, {"button", 0}, {"down", true}, {"x", 0.5}, {"y", 0.5}}),
        "hold button");
  check(send(message), "authorized unicode text");
  check(sink->events.size() == 5 && sink->events[2].code == "ControlLeft" &&
            !sink->events[2].down && sink->events[3].type == "button" && !sink->events[3].down &&
            sink->events[4].text == unicode,
        "release held inputs before text");
  check(
      session.status()["text_input"] == true && session.status()["text_result"]["accepted"] == true,
      "capability and text acknowledgement");
  check(!send(message) && sink->events.size() == 5, "fresh sequence cannot replay text id");
  message["text_id"] = 2u;
  message["text"] = "\x1b";
  check(!send(message) && session.status()["text_result"]["reason"] == "invalid_text",
        "invalid text acknowledgement");
  message["text"] = unicode;
  check(!send(message), "invalid request id is consumed");
  message["text_id"] = 3u;
  sink->text_enabled = false;
  check(!send(message) && session.status()["text_result"]["reason"] == "unsupported",
        "legacy sink refuses text");
  sink->text_enabled = true;
  message["text_id"] = 4u;
  sink->fail = true;
  check(!send(message) && !session.active(), "text failure stops grant");
  check(sink->events.back().type == "text_release", "failed text attempts release");
  bool rejected = false;
  try {
    session.authorize({"g2", "controller", 2, now + 10s}, {1, 0, 0, 100, 100}, now);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  check(rejected, "pending Unicode release prevents new grant");
  sink->fail = false;
  session.tick(now);
  check(sink->events.back().type == "text_release", "retry Unicode release after stop");
  session.authorize({"g", "controller", 1, now + 1s}, {1, 0, 0, 100, 100}, now);
  sequence = 0;
  check(send({{"type", "heartbeat"}}), "heartbeat after release");
  check(send({{"type", "layout_ack"}}), "ack after release");
  now += 1s;
  auto count = sink->events.size();
  check(!send(message) && sink->events.size() == count, "expired grant cannot inject text");
}

int main(int argc, char** argv) {
  try {
    handshake_tests();
    text_tests();
    auto sink = std::make_shared<Recorder>();
    RemoteControlSession session(sink, false);
    auto now = ControlClock::now();
    ControlGrant grant{"grant", "controller", 1, now + 10s};
    session.authorize(grant, {1, -100, 20, 1000, 500}, now);
    DataReceiveParams params{"controller", remote_control_topic, true};
    uint64_t seq = 0;
    auto send = [&](Json v) {
      v["v"] = 1;
      v["grant_id"] = "grant";
      v["epoch"] = 1;
      v["seq"] = ++seq;
      v["layout_version"] = 1;
      auto text = v.dump();
      return session.receive({text.begin(), text.end()}, params, now);
    };
    check(!send({{"type", "layout_ack"}}), "no layout ack before first heartbeat");
    check(send({{"type", "heartbeat"}}), "initial heartbeat");
    check(!send({{"type", "key"}, {"code", "KeyA"}, {"down", true}}),
          "requires layout acknowledgement");
    check(send({{"type", "layout_ack"}}), "ack");
    params.sender_identity = "intruder";
    check(!send({{"type", "key"}, {"code", "KeyA"}, {"down", true}}), "identity");
    params.sender_identity = "controller";
    check(send({{"type", "key"}, {"code", "KeyA"}, {"down", true}}), "key down");
    check(!send({{"type", "key"}, {"code", "FakeKey"}, {"down", true}}), "unknown key");
    check(send({{"type", "button"}, {"x", 1.0}, {"y", 0.0}, {"button", 0}, {"down", true}}),
          "button");
    check(sink->events.back().x == 899 && sink->events.back().y == 20, "coordinate mapping");
    check(!send({{"type", "move"}, {"x", 1.01}, {"y", 0.0}}), "bounds");
    check(!send({{"type", "button"},
                 {"x", 0.5},
                 {"y", 0.5},
                 {"button", 4294967296ULL},
                 {"down", true}}),
          "button integer overflow");
    check(!send({{"type", "wheel"}, {"x", 0.5}, {"y", 0.5}, {"delta", 1.5}}), "fractional wheel");
    session.tick(now + 3s);
    check(!session.active(), "heartbeat timeout");
    check(sink->events.size() == 4 && !sink->events[2].down && !sink->events[3].down,
          "release pressed keys and buttons");
    check(!session.renew(grant, now + 4s), "cannot resurrect");
    session.authorize(grant, {1, 0, 0, 100, 100}, now);
    seq = 0;
    check(send({{"type", "heartbeat"}}), "heartbeat again");
    check(send({{"type", "layout_ack"}}), "ack again");
    auto text =
        Json{{"v", 1}, {"grant_id", "grant"}, {"epoch", 1}, {"seq", 1}, {"type", "heartbeat"}}
            .dump();
    check(!session.receive({text.begin(), text.end()}, params, now), "replay");
    check(send({{"type", "key"}, {"code", "ShiftLeft"}, {"down", true}}), "shift");
    session.set_layout({2, 0, 0, 200, 200});
    check(!sink->events.back().down, "layout releases");
    check(!send({{"type", "move"}, {"x", 0.5}, {"y", 0.5}}), "stale layout");
    session.tick(now + 10s);
    check(session.reason() == "lease_expired", "expiry");
    session.authorize(grant, {1, 0, 0, 100, 100}, now);
    seq = 0;
    check(send({{"type", "heartbeat"}}), "heartbeat");
    check(send({{"type", "layout_ack"}}), "ack");
    sink->fail = true;
    check(!send({{"type", "key"}, {"code", "KeyB"}, {"down", true}}),
          "injection failure stops control");
    check(!session.active(), "failed input inactive");
    sink->fail = false;
    session.tick(now);
    check(sink->events.back().code == "KeyB" && !sink->events.back().down,
          "failed release retried");
    if (argc > 1) {
      std::ifstream file(argv[1]);
      Json fixture;
      file >> fixture;
      RemoteControlSession contract(sink, false);
      contract.authorize({"example", "controller_example", 1, now + 10s}, {1, 0, 0, 1920, 1080},
                         now);
      for (auto& message : fixture.at("controller_messages")) {
        auto raw = message.dump();
        check(contract.receive({raw.begin(), raw.end()},
                               {"controller_example", remote_control_topic, true}, now),
              "shared protocol fixture");
      }
    }
#ifdef _WIN32
    struct FakeApi : detail::WindowsInputApi {
      bool available = true;
      int limit = -1;
      unsigned calls = 0;
      std::vector<INPUT> inputs;

      bool desktop_available() noexcept override {
        return available;
      }

      ControlLayout virtual_layout() noexcept override {
        return {1, -1920, 0, 3840, 1080};
      }

      unsigned send(std::span<const INPUT> value) noexcept override {
        ++calls;
        inputs.assign(value.begin(), value.end());
        return limit < 0
                   ? static_cast<unsigned>(value.size())
                   : std::min(static_cast<unsigned>(limit), static_cast<unsigned>(value.size()));
      }
    };

    auto api = std::make_shared<FakeApi>();
    auto native = detail::windows_input_with_api(api);
    ControlInput input;
    input.type = "key";
    input.code = "ControlRight";
    input.down = true;
    check(native->apply(input), "native key");
    check(api->inputs[0].ki.wScan == 0x1d && (api->inputs[0].ki.dwFlags & KEYEVENTF_EXTENDEDKEY),
          "extended scan code");
    input.down = false;
    native->apply(input);
    check(api->inputs[0].ki.dwFlags & KEYEVENTF_KEYUP, "native keyup");
    input = {};
    input.type = "button";
    input.positioned = true;
    input.button = 2;
    input.down = true;
    input.x = 1919;
    input.y = 1079;
    check(native->apply(input), "native button");
    check(api->inputs.size() == 2 && api->inputs[0].mi.dx == 65535 &&
              api->inputs[0].mi.dy == 65535 && api->inputs[1].mi.dwFlags == MOUSEEVENTF_RIGHTDOWN,
          "virtual desktop normalization");
    input.positioned = false;
    input.down = false;
    native->apply(input);
    check(api->inputs.size() == 1 && api->inputs[0].mi.dwFlags == MOUSEEVENTF_RIGHTUP,
          "release does not move pointer");
    api->available = false;
    check(!native->apply(input), "secure desktop denied");
    input = {};
    input.type = "text";
    input.text = "\xe4\xbd\xa0\xf0\x9f\x98\x80\r\n\t";
    auto calls = api->calls;
    check(!native->apply(input) && api->calls == calls, "text secure desktop gate");
    api->available = true;
    check(native->supports_text() && native->apply(input), "native Unicode text");
    const std::u16string expected = u"\u4f60\U0001f600\r\t";
    check(api->inputs.size() == expected.size() * 2, "CRLF injected once");
    for (size_t i = 0; i < expected.size(); ++i) {
      const auto& down = api->inputs[i * 2];
      const auto& up = api->inputs[i * 2 + 1];
      check(down.type == INPUT_KEYBOARD && down.ki.wVk == 0 && down.ki.wScan == expected[i] &&
                down.ki.dwFlags == KEYEVENTF_UNICODE && up.ki.wScan == expected[i] &&
                up.ki.dwFlags == (KEYEVENTF_UNICODE | KEYEVENTF_KEYUP),
            "native Unicode pairs including surrogates");
    }
    input.text = "\xed\xa0\x80";
    calls = api->calls;
    check(!native->apply(input) && api->calls == calls, "native rejects invalid UTF-8 before send");
    input.text = "AB";
    api->limit = 1;
    check(!native->apply(input), "partial Unicode insertion fails");
    calls = api->calls;
    check(!native->apply(input) && api->calls == calls, "pending release blocks further text");
    ControlInput release;
    release.type = "text_release";
    api->limit = 0;
    check(!native->apply(release), "failed Unicode keyup retained");
    api->available = false;
    check(!native->apply(release), "secure desktop blocks pending release");
    api->available = true;
    api->limit = -1;
    check(native->apply(release) && api->inputs.size() == 1 && api->inputs[0].ki.wScan == 'A' &&
              api->inputs[0].ki.dwFlags == (KEYEVENTF_UNICODE | KEYEVENTF_KEYUP),
          "retry releases unmatched Unicode key");
    calls = api->calls;
    check(native->apply(release) && api->calls == calls, "release idempotent");
    api->limit = 2;
    check(!native->apply(input), "partial complete character still fails request");
    calls = api->calls;
    check(native->apply(release) && api->calls == calls,
          "no keyup needed after complete partial pair");
    api->limit = -1;
    input.text.clear();
    for (int i = 0; i < 256; ++i)
      input.text += "\xf0\x9f\x98\x80";
    check(native->apply(input) && api->inputs.size() == 1024, "maximum Unicode input batch");
#endif
    std::cout << "remote control tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
