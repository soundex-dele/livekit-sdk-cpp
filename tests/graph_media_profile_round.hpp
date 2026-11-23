#pragma once
#include <filesystem>
#include <functional>

// Test fixture only. Rendezvous after encoder setup and again after player setup.
using MediaPhaseReady = std::function<bool()>;
int mediaProfile(const std::filesystem::path& root, int frames, const MediaPhaseReady& ready);
