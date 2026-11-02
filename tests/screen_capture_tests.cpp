#include <livekit/media.hpp>
#include "media_nodes/capture/screen_selection.hpp"
#include <iostream>
#include <array>
#include "rtc_result.hpp"
#include "test_failure.hpp"
using livekit::sdk::app_support::require_result;

int main() {
  {
    auto check = [](bool ok) {
      if (!ok)
        livekit::sdk::test::fail("screen capture assertion");
    };
    using namespace livekit::sdk;
    for (auto [w, h, ow, oh] : std::vector<std::array<int, 4>>{{3840, 2160, 1920, 1080},
                                                               {2160, 3840, 1080, 1920},
                                                               {1280, 720, 1280, 720},
                                                               {1023, 767, 1022, 766},
                                                               {3440, 1440, 1920, 802}}) {
      auto size = require_result(screen_capture_size(w, h));
      check(size.width == ow && size.height == oh);
    }
    std::vector<ScreenCaptureDevice> screens{{"primary", "main", 0, 0, 1920, 1080, true},
                                             {"left", "portrait", -1080, -300, 1080, 1920, false}};
    check(require_result(lk::select_screen(screens, "")).id == "primary");
    auto selected = require_result(lk::select_screen(screens, "left"));
    check(selected.x == -1080 && selected.y == -300);
    require_result(lk::validate_screen(selected, screens));
    screens[1].height = 1600;
    check(!lk::validate_screen(selected, screens));
    screens.pop_back();
    check(!lk::validate_screen(selected, screens));
    std::cout << "screen sizing and display selection passed (simulated displays)\n";
    return 0;
  }
}
