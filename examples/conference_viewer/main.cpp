#define SDL_MAIN_HANDLED
#include "ui.hpp"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include <algorithm>
#include <iostream>

namespace {
float ui_scale(int display) {
#ifdef __APPLE__
  // Cocoa window sizes and input coordinates already use logical points.
  return 1.0f;
#else
  float dpi = 96;
  if (SDL_GetDisplayDPI(display, &dpi, nullptr, nullptr) != 0)
    dpi = 96;
  return std::clamp(dpi / 96, 1.0f, 3.0f);
#endif
}

float pixel_density(SDL_Window* window, SDL_Renderer* renderer) {
  int w = 0, h = 0, pixels_w = 0, pixels_h = 0;
  SDL_GetWindowSize(window, &w, &h);
  if (w <= 0 || h <= 0 || SDL_GetRendererOutputSize(renderer, &pixels_w, &pixels_h) != 0)
    return 1.0f;
  return std::clamp(std::max(float(pixels_w) / w, float(pixels_h) / h), 1.0f, 3.0f);
}
}  // namespace

int main() {
  SDL_SetMainReady();
  SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
  SDL_Window* window = nullptr;
  SDL_Renderer* renderer = nullptr;
  bool platform = false, backend = false;
  using namespace livekit::sdk;
  auto failed = [](std::string message) {
    return Result<void>::failure(
        make_failure(ErrorDomain::media, ErrorCode::processing_failed, std::move(message)));
  };
  auto run = [&]() -> Result<void> {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
      return failed(SDL_GetError());
    float scale = ui_scale(0);
    window = SDL_CreateWindow("LiveKit Conference", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              static_cast<int>(1100 * scale), static_cast<int>(720 * scale),
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window)
      return failed(SDL_GetError());
    SDL_SetWindowMinimumSize(window, 640, 480);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer)
      renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer)
      return failed(SDL_GetError());
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    float density = pixel_density(window, renderer);
    demo::apply_style(scale, density);
    platform = ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    backend = ImGui_ImplSDLRenderer2_Init(renderer);
    if (!platform || !backend)
      return failed("Cannot initialize ImGui SDL backends");
    {
      demo::Session session;
      demo::View view;
      bool closing = false;
      for (;;) {
        auto started = SDL_GetTicks64();
        SDL_Event input;
        while (SDL_PollEvent(&input)) {
          ImGui_ImplSDL2_ProcessEvent(&input);
          if (input.type == SDL_QUIT ||
              (input.type == SDL_WINDOWEVENT && input.window.event == SDL_WINDOWEVENT_CLOSE)) {
            closing = true;
            session.stop();
          }
        }
        // A backing-scale transition can accompany a resize or screen move.
        const float next_scale = ui_scale(SDL_GetWindowDisplayIndex(window));
        const float next_density = pixel_density(window, renderer);
        if (next_scale != scale || next_density != density) {
          scale = next_scale;
          density = next_density;
          ImGui_ImplSDLRenderer2_DestroyFontsTexture();
          demo::apply_style(scale, density);
        }
        auto snapshot = session.snapshot();
        if (closing && snapshot.finished)
          break;
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        const auto action = view.draw(renderer, snapshot);
        if (action == demo::Action::Leave)
          session.stop();
        if (action == demo::Action::AddVideo && !closing)
          session.add_video();
        if (action == demo::Action::RemoveVideo && !closing)
          session.remove_video(view.remove_video_sid);
        if (action == demo::Action::Join && !closing) {
          auto options = view.options();
          if (!options) {
            view.error = options.error().message;
          } else if (auto started = session.start(std::move(*options), view.pause_unused_video);
                     !started) {
            view.error = started.error().message;
          }
        }
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 14, 17, 23, 255);
        SDL_RenderClear(renderer);
        if (auto rendered = demo::render_draw_data(renderer); !rendered) {
          return rendered;
        }
        SDL_RenderPresent(renderer);
        const auto elapsed = SDL_GetTicks64() - started;
        if (elapsed < 16)
          SDL_Delay(static_cast<Uint32>(16 - elapsed));
      }
    }
    return Result<void>::success();
  };
  auto completed = run();
  if (!completed) {
    std::cerr << "Conference: " << completed.error().message << '\n';
  }
  if (backend)
    ImGui_ImplSDLRenderer2_Shutdown();
  if (platform)
    ImGui_ImplSDL2_Shutdown();
  if (ImGui::GetCurrentContext())
    ImGui::DestroyContext();
  if (renderer)
    SDL_DestroyRenderer(renderer);
  if (window)
    SDL_DestroyWindow(window);
  SDL_Quit();
  return completed ? 0 : 1;
}
