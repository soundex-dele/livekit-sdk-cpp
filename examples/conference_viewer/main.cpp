#define SDL_MAIN_HANDLED
#include "ui.hpp"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include <algorithm>
#include <iostream>

int main() {
  SDL_SetMainReady();
  SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
  SDL_Window* window = nullptr; SDL_Renderer* renderer = nullptr;
  bool platform = false, backend = false;
  int result = 0;
  try {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) throw std::runtime_error(SDL_GetError());
    float dpi = 96; if (SDL_GetDisplayDPI(0, &dpi, nullptr, nullptr) != 0) dpi = 96;
    float scale = std::clamp(dpi / 96, 1.0f, 3.0f);
    window = SDL_CreateWindow("LiveKit Conference", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      static_cast<int>(1100 * scale), static_cast<int>(720 * scale), SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) throw std::runtime_error(SDL_GetError());
    SDL_SetWindowMinimumSize(window, 640, 480);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) throw std::runtime_error(SDL_GetError());
    IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    demo::apply_style(scale);
    platform = ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    backend = ImGui_ImplSDLRenderer2_Init(renderer);
    if (!platform || !backend) throw std::runtime_error("Cannot initialize ImGui SDL backends");
    {
      demo::Session session; demo::View view;
      bool closing = false;
      for (;;) {
        auto started = SDL_GetTicks64();
        SDL_Event input;
        while (SDL_PollEvent(&input)) {
          ImGui_ImplSDL2_ProcessEvent(&input);
          if (input.type == SDL_QUIT || (input.type == SDL_WINDOWEVENT && input.window.event == SDL_WINDOWEVENT_CLOSE)) { closing = true; session.stop(); }
          if (input.type == SDL_WINDOWEVENT && input.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) {
            float next = 96; if (SDL_GetDisplayDPI(SDL_GetWindowDisplayIndex(window), &next, nullptr, nullptr) == 0) {
              const float next_scale = std::clamp(next / 96, 1.0f, 3.0f);
              if (next_scale != scale) { scale = next_scale; demo::apply_style(scale); }
            }
          }
        }
        auto snapshot = session.snapshot();
        if (closing && snapshot.finished) break;
        ImGui_ImplSDLRenderer2_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame();
        const auto action = view.draw(renderer, snapshot);
        if (action == demo::Action::Leave) session.stop();
        if (action == demo::Action::AddVideo && !closing) session.add_video();
        if (action == demo::Action::RemoveVideo && !closing) session.remove_video(view.remove_video_sid);
        if (action == demo::Action::Join && !closing) {
          try { session.start(view.options()); }
          catch (const std::exception& e) { view.error = e.what(); }
        }
        ImGui::Render(); SDL_SetRenderDrawColor(renderer, 14, 17, 23, 255); SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer); SDL_RenderPresent(renderer);
        const auto elapsed = SDL_GetTicks64() - started; if (elapsed < 16) SDL_Delay(static_cast<Uint32>(16 - elapsed));
      }
    }
  } catch (const std::exception& error) { std::cerr << "Conference: " << error.what() << '\n'; result = 1; }
  if (backend) ImGui_ImplSDLRenderer2_Shutdown();
  if (platform) ImGui_ImplSDL2_Shutdown();
  if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
  if (renderer) SDL_DestroyRenderer(renderer); if (window) SDL_DestroyWindow(window);
  SDL_Quit(); return result;
}
