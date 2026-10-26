#define SDL_MAIN_HANDLED
#include "ui.hpp"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include <filesystem>
#include <iostream>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}
int main(int argc, char** argv) {
  try {
    require(argc == 2, "fixture directory required");
    SDL_SetMainReady(); require(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == 0, "SDL init");
    auto* window = SDL_CreateWindow("Player UI test", 0, 0, 1080, 740, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
    require(window != nullptr, "window");
    auto* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    require(renderer != nullptr, "renderer");
    ImGui::CreateContext(); ImGui::GetIO().IniFilename = nullptr;
    player_demo::load_font(1); player_demo::apply_style(1);
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer); ImGui_ImplSDLRenderer2_Init(renderer);
    {
      player_demo::Session session(renderer); player_demo::View view;
      auto render = [&] {
        session.update();
        ImGui_ImplSDLRenderer2_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame();
        view.draw(session, window); ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
      };
      auto wait = [&](auto condition) {
        const auto limit = SDL_GetTicks64() + 5000;
        while (!condition() && SDL_GetTicks64() < limit) { render(); SDL_Delay(2); }
        require(condition(), "UI operation timed out");
      };
      const std::filesystem::path fixtures = argv[1];
      auto file = [&](const char* name) { auto value = (fixtures / name).u8string(); return std::string(value.begin(), value.end()); };
      render(); render(); // empty state, without a command-line media file
      session.open(file("av.mp4"));
      wait([&] { return !session.busy() && session.texture(); });
      require(session.snapshot().state == player_demo::sdk::PlayerState::playing, "open did not autoplay");
      require(session.video_width() == 160 && session.video_height() == 90, "texture dimensions");
      std::vector<uint32_t> pixels(1080 * 740);
      render();
      require(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, pixels.data(), 1080 * 4) == 0, "readback");
      int colorful = 0;
      for (int y = 100; y < 490; ++y) for (int x = 30; x < 1050; ++x) {
        const auto p = pixels[y * 1080 + x];
        if (((p >> 16) & 255) > 180 && ((p >> 8) & 255) < 100) ++colorful;
      }
      require(colorful > 5000, "video texture was not rendered inside ImGui");
      bool paused = false;
      session.pause([&] { paused = true; });
      wait([&] { return !session.busy(); });
      require(paused && session.snapshot().state == player_demo::sdk::PlayerState::paused, "pause completion callback");
      const auto position = session.snapshot().position_us;
      SDL_Delay(35); render();
      require(session.snapshot().position_us == position && session.texture(), "paused picture or clock changed");
      session.seek(1200000); wait([&] { return !session.busy(); });
      require(!session.texture() && session.snapshot().state == player_demo::sdk::PlayerState::paused, "seek retained stale image");
      session.muted = true; session.volume = .25f;
      session.toggle_play(); wait([&] { return !session.busy() && session.texture(); });
      require(session.snapshot().position_us >= 1200000, "seek target not applied");
      session.stop(); wait([&] { return !session.busy(); });
      require(!session.texture() && session.snapshot().position_us == 0, "stop failed to clear output");
      session.open(file("audio.wav")); wait([&] { return !session.busy(); });
      require(session.snapshot().media.has_audio && !session.snapshot().media.has_video && !session.texture(), "file switch retained video");
      wait([&] { return session.drained(); });
      session.open(file("invalid.mp4")); wait([&] { return !session.busy(); });
      require(!session.error().empty(), "invalid file had no visible error"); render();
      // Error recovery and a narrow window must continue to render correctly.
      SDL_SetWindowSize(window, 800, 520);
      session.open(file("video.mp4")); wait([&] { return !session.busy() && session.texture(); });
      require(session.error().empty(), "reopen did not clear the error"); render();
      session.close(); wait([&] { return session.finished(); });
      require(!session.texture(), "close retained video");
    }
    // Closing while open is still pending must complete without a blocking UI wait.
    {
      player_demo::Session session(renderer);
      session.open((std::filesystem::path(argv[1]) / "av.mp4").string()); session.close();
      const auto limit = SDL_GetTicks64() + 5000;
      while (!session.finished() && SDL_GetTicks64() < limit) { session.update(); SDL_Delay(2); }
      require(session.finished(), "close during open timed out");
    }
    ImGui_ImplSDLRenderer2_Shutdown(); ImGui_ImplSDL2_Shutdown(); ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
    std::cout << "PASS: ImGui video rendering, async controls, seek, file switching, error recovery, closing\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
