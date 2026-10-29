#pragma once
#include "livekit/media.hpp"
#include "livekit/json.hpp"
#include <filesystem>
#include <stop_token>
namespace livekit::cli {
struct SessionOptions {
  std::string url,token,room,identity,codec="vp8",publish_data,topic;
  std::vector<sdk::MediaSpec> media;
  std::vector<std::string> subscribe_tracks;
  std::filesystem::path record;
  bool subscribe=true,microphone=false,audio_playout=false,video_playout=false,exit_after_publish=false;
  int duration=0,connect_timeout=30,stats_interval_ms=0;
};
void run_session(const SessionOptions&,std::function<void(const sdk::Json&)>,std::stop_token);
void doctor(std::function<void(const sdk::Json&)>);
}
