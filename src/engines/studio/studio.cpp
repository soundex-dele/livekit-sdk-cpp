#include "internal.hpp"
#include "studio_graph.hpp"
#include "support/utf8.hpp"
#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#endif
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <set>
#include <thread>

namespace livekit::sdk {
using namespace studio_detail;
using Json = nlohmann::json;

namespace {
Result<void> validate(const StudioScene& s) {
  if (s.sources.size() > 32)
    return failed("最多支持 32 个素材", ErrorCode::invalid_argument);
  if (!detail::valid_utf8(s.id) || !detail::valid_utf8(s.name))
    return failed("Invalid scene UTF-8", ErrorCode::invalid_argument);
  std::set<std::string> ids;
  for (const auto& v : s.sources) {
    if (v.id.empty() || !ids.insert(v.id).second)
      return failed("素材 ID 无效或重复", ErrorCode::invalid_argument);
    if (!detail::valid_utf8(v.id) || !detail::valid_utf8(v.name) || !detail::valid_utf8(v.input))
      return failed("Invalid source UTF-8", ErrorCode::invalid_argument);
    if (int(v.kind) < 0 || int(v.kind) > int(StudioSourceKind::remote))
      return failed("Invalid source kind", ErrorCode::invalid_argument);
    for (float value : v.chroma.color)
      if (!std::isfinite(value) || value < 0 || value > 1)
        return failed("Invalid chroma color", ErrorCode::invalid_argument);
    auto& t = v.transform;
    for (float f :
         {t.x, t.y, t.width, t.height, t.rotation, t.crop_left, t.crop_top, t.crop_right,
          t.crop_bottom, v.volume, v.chroma.similarity, v.chroma.smoothness, v.chroma.spill})
      if (!std::isfinite(f))
        return failed("无效素材参数", ErrorCode::invalid_argument);
    if (t.width <= 0 || t.height <= 0 || t.width > 8 || t.height > 8 || std::abs(t.x) > 8 ||
        std::abs(t.y) > 8 || t.crop_left < 0 || t.crop_right < 0 || t.crop_top < 0 ||
        t.crop_bottom < 0 || t.crop_left + t.crop_right >= 1 || t.crop_top + t.crop_bottom >= 1 ||
        v.volume < 0 || v.volume > 4)
      return failed("素材变换或音量超出范围", ErrorCode::invalid_argument);
    for (float f : v.color)
      if (!std::isfinite(f) || f < 0 || f > 1)
        return failed("无效颜色", ErrorCode::invalid_argument);
  }
  return Result<void>::success();
}

bool number_array(const Json& value, size_t count) {
  if (!value.is_array() || value.size() != count)
    return false;
  for (const auto& x : value)
    if (!x.is_number())
      return false;
  return true;
}

bool fields(const Json& value, std::initializer_list<const char*> strings,
            std::initializer_list<const char*> booleans,
            std::initializer_list<const char*> numbers) {
  if (!value.is_object())
    return false;
  for (auto name : strings)
    if (!value.contains(name) || !value[name].is_string())
      return false;
  for (auto name : booleans)
    if (!value.contains(name) || !value[name].is_boolean())
      return false;
  for (auto name : numbers)
    if (!value.contains(name) || !value[name].is_number())
      return false;
  return true;
}

bool project_schema(const Json& j) {
  if (!fields(j, {}, {}, {"version", "selected", "width", "height"}) ||
      !j["version"].is_number_integer() || !j["selected"].is_number_unsigned() ||
      !j["width"].is_number_integer() || !j["height"].is_number_integer() ||
      !j.contains("scenes") || !j["scenes"].is_array() || j["scenes"].size() > 20 ||
      !j.contains("audio") ||
      !fields(j["audio"], {}, {"microphone", "system", "monitor"},
              {"mic_volume", "system_volume", "media_volume", "remote_volume"}))
    return false;
  for (const auto& scene : j["scenes"]) {
    if (!fields(scene, {"id", "name"}, {}, {}) || !scene.contains("sources") ||
        !scene["sources"].is_array() || scene["sources"].size() > 32)
      return false;
    for (const auto& source : scene["sources"]) {
      if (!fields(source, {"id", "name", "input"}, {"visible", "locked", "loop", "muted"},
                  {"kind", "volume"}) ||
          !source["kind"].is_number_integer() || !source.contains("transform") ||
          !number_array(source["transform"], 9) || !source.contains("color") ||
          !number_array(source["color"], 4) || !source.contains("chroma"))
        return false;
      const auto& chroma = source["chroma"];
      if (!fields(chroma, {}, {"enabled"}, {"similarity", "smoothness", "spill"}) ||
          !chroma.contains("color") || !number_array(chroma["color"], 3))
        return false;
    }
  }
  return true;
}

void append(std::deque<float>& q, const std::vector<float>& p) {
  q.insert(q.end(), p.begin(), p.end());
  while (q.size() > 19200)
    q.pop_front();
}
}  // namespace

struct Studio::Impl : StudioGraphState {
  std::atomic<bool> stop{false};
  std::jthread worker;

  explicit Impl(StudioOptions options) : StudioGraphState(options), worker([this] { run(); }) {}

  ~Impl() {
    stop = true;
    if (worker.joinable())
      worker.join();
    if (programVideo)
      programVideo->close();
    if (localVideo)
      localVideo->close();
    if (programAudio)
      programAudio->close();
    if (localAudio)
      localAudio->close();
  }

  void run() {
#ifdef _WIN32
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
    auto execute = [&]() -> Result<void> {
#ifdef _WIN32
      if (FAILED(com))
        return failed("Studio COM initialization failed", ErrorCode::processing_failed, int(com));
#endif
      {
        std::lock_guard lock(mutex);
        if (status.failure)
          return failed(status.failure);
      }
      StudioGraph graph(*this);
      LK_STUDIO_TRY(graph.start());
      while (!stop) {
        auto result = graph.step();
        if (!result) {
          graph.stop();
          return failed(result.failure_record());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      graph.stop();
      return Result<void>::success();
    };
    auto result = execute();
    {
      std::lock_guard lock(mutex);
      if (!result) {
        status.failure = result.failure_record();
        status.error = result.error().message;
      }
      status.recording = false;
    }
#ifdef _WIN32
    if (SUCCEEDED(com))
      CoUninitialize();
#endif
  }
};

Studio::Studio(StudioOptions o) : impl_(std::make_unique<Impl>(o)) {}

Studio::~Studio() = default;

Result<void> Studio::set_scene(StudioScene s) {
  LK_STUDIO_TRY(validate(s));
  std::lock_guard lock(impl_->mutex);
  impl_->scene = std::move(s);
  return Result<void>::success();
}

Result<void> Studio::set_audio(StudioAudioSettings a) {
  for (float v : {a.microphone_volume, a.system_volume, a.media_volume, a.remote_volume})
    if (!std::isfinite(v) || v < 0 || v > 2)
      return failed("无效混音音量", ErrorCode::invalid_argument);
  std::lock_guard lock(impl_->mutex);
  impl_->settings = a;
  return Result<void>::success();
}

Result<void> Studio::submit_remote_video(std::string id, const VideoFrame& f) {
  if (f.width < 1 || f.height < 1 || f.width > 3840 || f.height > 3840 || f.stride_y < f.width ||
      f.stride_u < (f.width + 1) / 2 || f.stride_v < (f.width + 1) / 2 ||
      f.y.size() < size_t(f.stride_y) * f.height ||
      f.u.size() < size_t(f.stride_u) * ((f.height + 1) / 2) ||
      f.v.size() < size_t(f.stride_v) * ((f.height + 1) / 2))
    return failed("无效远端视频", ErrorCode::invalid_argument);
  LK_STUDIO_ASSIGN(image, from_i420(f));
  std::lock_guard lock(impl_->mutex);
  if (impl_->remoteVideo.size() < 4 || impl_->remoteVideo.contains(id)) {
    impl_->remoteVideo[id] = std::move(image);
    impl_->remoteSeen[id] = std::chrono::steady_clock::now();
  }
  return Result<void>::success();
}

Result<void> Studio::submit_remote_audio(std::string id, const AudioFrame& f) {
  if (f.sample_rate <= 0 || f.channels < 1 || f.channels > 2 || f.samples_per_channel == 0 ||
      f.samples_per_channel > size_t(f.sample_rate) ||
      (f.format != AudioSampleFormat::pcm16 && f.format != AudioSampleFormat::float32))
    return failed("Invalid remote audio", ErrorCode::invalid_argument);
  size_t count = f.format == AudioSampleFormat::pcm16 ? f.samples.size() : f.float_samples.size();
  if (count < f.samples_per_channel * f.channels)
    return failed("Invalid remote audio", ErrorCode::invalid_argument);
  size_t n = f.samples_per_channel * 48000 / f.sample_rate;
  if (n > 48000)
    return failed("Invalid remote audio", ErrorCode::invalid_argument);
  std::vector<float> samples(n * 2);
  for (size_t i = 0; i < n; ++i)
    for (size_t c = 0; c < 2; ++c) {
      size_t p =
          std::min(f.samples_per_channel - 1, i * size_t(f.sample_rate) / 48000) * f.channels +
          std::min(c, f.channels - 1);
      samples[i * 2 + c] =
          f.format == AudioSampleFormat::pcm16 ? f.samples[p] / 32768.f : f.float_samples[p];
    }
  std::lock_guard lock(impl_->mutex);
  if (impl_->remoteAudio.size() < 4 || impl_->remoteAudio.contains(id)) {
    append(impl_->remoteAudio[id], samples);
    impl_->remoteSeen[id] = std::chrono::steady_clock::now();
  }
  return Result<void>::success();
}

void Studio::remove_remote(const std::string& id) {
  std::lock_guard lock(impl_->mutex);
  impl_->remoteVideo.erase(id);
  impl_->remoteAudio.erase(id);
  impl_->remoteSeen.erase(id);
}

void Studio::clear_remote() {
  std::lock_guard lock(impl_->mutex);
  impl_->remoteVideo.clear();
  impl_->remoteAudio.clear();
  impl_->remoteSeen.clear();
}

Result<void> Studio::start_recording(std::filesystem::path p) {
  if (p.extension() != L".mp4")
    return failed("录制文件需要 .mp4 扩展名", ErrorCode::invalid_argument);
  std::lock_guard lock(impl_->mutex);
  if (impl_->recordWanted || impl_->status.recording)
    return failed("正在录制", ErrorCode::busy);
  impl_->status.error.clear();
  impl_->status.failure.reset();
  impl_->recordPath = std::move(p);
  impl_->recordWanted = true;
  return Result<void>::success();
}

void Studio::stop_recording() {
  std::lock_guard lock(impl_->mutex);
  impl_->recordWanted = false;
}

Result<void> Studio::status() const {
  std::lock_guard lock(impl_->mutex);
  if (impl_->status.failure)
    return failed(impl_->status.failure);
  return Result<void>::success();
}

StudioSnapshot Studio::snapshot() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->status;
}

StudioOptions Studio::options() const {
  return impl_->options;
}

std::shared_ptr<I420VideoSource> Studio::video_output() const {
  return impl_->programVideo;
}

std::shared_ptr<PcmAudioSource> Studio::audio_output() const {
  return impl_->programAudio;
}

std::shared_ptr<I420VideoSource> Studio::local_video_output() const {
  return impl_->localVideo;
}

std::shared_ptr<PcmAudioSource> Studio::local_audio_output() const {
  return impl_->localAudio;
}

Result<void> Studio::save_project(const std::filesystem::path& p,
                                  const std::vector<StudioScene>& scenes, size_t selected,
                                  StudioOptions o, StudioAudioSettings a) {
  if (scenes.empty() || scenes.size() > 20 || selected >= scenes.size())
    return failed("无效场景集合", ErrorCode::invalid_argument);
  if ((o.width != 1280 || o.height != 720) && (o.width != 720 || o.height != 1280) &&
      (o.width != 1920 || o.height != 1080) && (o.width != 1080 || o.height != 1920))
    return failed("Invalid canvas dimensions", ErrorCode::invalid_argument);
  for (float gain : {a.microphone_volume, a.system_volume, a.media_volume, a.remote_volume})
    if (!std::isfinite(gain) || gain < 0 || gain > 2)
      return failed("Invalid audio gain", ErrorCode::invalid_argument);
  Json j{{"version", 1},
         {"selected", selected},
         {"width", o.width},
         {"height", o.height},
         {"audio",
          {{"microphone", a.microphone},
           {"system", a.system},
           {"monitor", a.monitor},
           {"mic_volume", a.microphone_volume},
           {"system_volume", a.system_volume},
           {"media_volume", a.media_volume},
           {"remote_volume", a.remote_volume}}},
         {"scenes", Json::array()}};
  for (auto& scene : scenes) {
    LK_STUDIO_TRY(validate(scene));
    Json items = Json::array();
    for (auto& s : scene.sources) {
      if (s.kind == StudioSourceKind::remote)
        continue;
      auto& t = s.transform;
      items.push_back({{"id", s.id},
                       {"name", s.name},
                       {"kind", int(s.kind)},
                       {"input", s.input},
                       {"transform",
                        {t.x, t.y, t.width, t.height, t.rotation, t.crop_left, t.crop_top,
                         t.crop_right, t.crop_bottom}},
                       {"color", s.color},
                       {"visible", s.visible},
                       {"locked", s.locked},
                       {"loop", s.loop},
                       {"muted", s.muted},
                       {"volume", s.volume},
                       {"chroma",
                        {{"enabled", s.chroma.enabled},
                         {"color", s.chroma.color},
                         {"similarity", s.chroma.similarity},
                         {"smoothness", s.chroma.smoothness},
                         {"spill", s.chroma.spill}}}});
    }
    j["scenes"].push_back({{"id", scene.id}, {"name", scene.name}, {"sources", items}});
  }
  std::error_code ec;
  if (!p.parent_path().empty()) {
    std::filesystem::create_directories(p.parent_path(), ec);
    if (ec)
      return failed(ec.message(), ErrorCode::processing_failed, ec.value());
  }
  auto tmp = p;
  tmp += L".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f || !(f << j.dump(2)))
      return failed("无法保存场景", ErrorCode::processing_failed);
    f.flush();
    if (!f)
      return failed("场景写入失败", ErrorCode::processing_failed);
  }
#ifdef _WIN32
  if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    return failed("无法替换场景文件", ErrorCode::processing_failed, int(GetLastError()));
#else
  std::filesystem::rename(tmp, p, ec);
  if (ec)
    return failed(ec.message(), ErrorCode::processing_failed, ec.value());
#endif
  return Result<void>::success();
}

Result<std::vector<StudioScene>> Studio::load_project(const std::filesystem::path& p,
                                                      size_t& selected, StudioOptions& o,
                                                      StudioAudioSettings& a) {
  std::error_code ec;
  auto size = std::filesystem::file_size(p, ec);
  if (ec)
    return failed(ec.message(), ErrorCode::processing_failed, ec.value());
  if (size > 2 * 1024 * 1024)
    return failed("场景文件过大", ErrorCode::invalid_argument);
  std::ifstream f(p);
  if (!f)
    return failed("Cannot open studio project", ErrorCode::processing_failed);
  auto j = Json::parse(f, nullptr, false);
  if (!project_schema(j))
    return failed("Invalid studio project schema", ErrorCode::invalid_argument);
  if (j.at("version") != 1)
    return failed("不支持的场景版本", ErrorCode::invalid_argument);
  std::vector<StudioScene> out;
  for (auto& v : j.at("scenes")) {
    StudioScene scene;
    scene.id = v.at("id");
    scene.name = v.at("name");
    for (auto& x : v.at("sources")) {
      StudioSource s;
      s.id = x.at("id");
      s.name = x.at("name");
      if (x["kind"] < 0 || x["kind"] >= int(StudioSourceKind::remote))
        return failed("Invalid source kind", ErrorCode::invalid_argument);
      int kind = x.at("kind");
      if (kind < 0 || kind >= int(StudioSourceKind::remote))
        return failed("无效素材类型", ErrorCode::invalid_argument);
      s.kind = StudioSourceKind(kind);
      s.input = x.at("input");
      auto t = x.at("transform").get<std::array<float, 9>>();
      s.transform = {t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8]};
      s.color = x.at("color").get<std::array<float, 4>>();
      s.visible = x.at("visible");
      s.locked = x.at("locked");
      s.loop = x.at("loop");
      s.muted = x.at("muted");
      s.volume = x.at("volume");
      auto& k = x.at("chroma");
      s.chroma.enabled = k.at("enabled");
      s.chroma.color = k.at("color").get<std::array<float, 3>>();
      s.chroma.similarity = k.at("similarity");
      s.chroma.smoothness = k.at("smoothness");
      s.chroma.spill = k.at("spill");
      scene.sources.push_back(s);
    }
    LK_STUDIO_TRY(validate(scene));
    out.push_back(scene);
  }
  auto sel = j.at("selected").get<size_t>();
  if (out.empty() || out.size() > 20 || sel >= out.size())
    return failed("无效场景集合", ErrorCode::invalid_argument);
  StudioAudioSettings audio;
  auto& v = j.at("audio");
  audio.microphone = v.at("microphone");
  audio.system = v.at("system");
  audio.monitor = v.at("monitor");
  audio.microphone_volume = v.at("mic_volume");
  audio.system_volume = v.at("system_volume");
  audio.media_volume = v.at("media_volume");
  audio.remote_volume = v.at("remote_volume");
  if (!((j["width"] == 1280 && j["height"] == 720) || (j["width"] == 720 && j["height"] == 1280) ||
        (j["width"] == 1920 && j["height"] == 1080) || (j["width"] == 1080 && j["height"] == 1920)))
    return failed("Invalid canvas dimensions", ErrorCode::invalid_argument);
  int width = j.at("width"), height = j.at("height");
  if ((width != 1280 || height != 720) && (width != 720 || height != 1280) &&
      (width != 1920 || height != 1080) && (width != 1080 || height != 1920))
    return failed("无效画布尺寸", ErrorCode::invalid_argument);
  for (float gain :
       {audio.microphone_volume, audio.system_volume, audio.media_volume, audio.remote_volume})
    if (!std::isfinite(gain) || gain < 0 || gain > 2)
      return failed("无效音频配置", ErrorCode::invalid_argument);
  o.width = width;
  o.height = height;
  a = audio;
  selected = sel;
  return Result<std::vector<StudioScene>>::success(std::move(out));
}
}  // namespace livekit::sdk
