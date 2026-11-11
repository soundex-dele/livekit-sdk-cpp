#include "editor_internal.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace livekit::sdk {
using editor_detail::failure;
using Json = nlohmann::json;

namespace {
constexpr int64_t kMaxFrames = 100000000;

bool finite(double value, double low, double high) {
  return std::isfinite(value) && value >= low && value <= high;
}

const EditorAsset* asset_for(const EditorProject& project, const std::string& id) {
  for (const auto& asset : project.assets) {
    if (asset.id == id) {
      return &asset;
    }
  }
  return nullptr;
}

// Validate JSON before typed access; malformed external input never throws.
bool fields(const Json& object, const char* strings, const char* numbers, const char* bools) {
  if (!object.is_object()) {
    return false;
  }
  auto check = [&](const char* names, auto predicate) {
    std::istringstream stream(names);
    std::string name;
    while (stream >> name) {
      const auto it = object.find(name);
      if (it == object.end() || !predicate(*it)) {
        return false;
      }
    }
    return true;
  };
  return check(strings, [](const Json& j) { return j.is_string(); }) &&
         check(numbers, [](const Json& j) { return j.is_number(); }) &&
         check(bools, [](const Json& j) { return j.is_boolean(); });
}

bool integer(const Json& j, int64_t low, int64_t high) {
  return j.is_number_integer() && j.get<double>() >= low && j.get<double>() <= high;
}
}  // namespace

int64_t editor_time_us(int64_t frame, EditorRate rate) {
  if (rate.numerator <= 0 || rate.denominator <= 0 || frame < 0 || frame > kMaxFrames ||
      rate.numerator > 240000 || rate.denominator > rate.numerator ||
      rate.numerator > int64_t(rate.denominator) * 120) {
    return 0;
  }
  const auto ticks = frame * int64_t(rate.denominator);
  return (ticks / rate.numerator) * 1000000 + (ticks % rate.numerator) * 1000000 / rate.numerator;
}

int64_t editor_duration(const EditorProject& project) {
  int64_t end = 0;
  for (const auto& track : project.tracks) {
    for (const auto& clip : track.clips) {
      end = std::max(end, clip.position + clip.duration);
    }
  }
  return end;
}

Result<void> editor_validate(const EditorProject& p) {
  if (p.width < 2 || p.height < 2 || p.width > 4096 || p.height > 4096 || p.width % 2 ||
      p.height % 2 || p.rate.numerator < 1 || p.rate.numerator > 240000 || p.rate.denominator < 1 ||
      p.rate.denominator > 1001000 || double(p.rate.numerator) / p.rate.denominator < 1 ||
      double(p.rate.numerator) / p.rate.denominator > 120 || p.assets.size() > 10000 ||
      p.tracks.size() > 64) {
    return Result<void>::failure(failure("project: invalid canvas, frame rate or resource limit",
                                         ErrorCode::invalid_argument));
  }
  std::set<std::string> assets, tracks, clips;
  for (const auto& a : p.assets) {
    if (a.id.empty() || !assets.insert(a.id).second || a.path.empty() ||
        (!a.video && !a.audio && !a.image) || a.duration_us < 0 ||
        a.duration_us > INT64_C(100000000000000) || a.width < 0 || a.height < 0 || a.width > 4096 ||
        a.height > 4096 || (a.video && (!a.width || !a.height)) || (a.image && !a.video)) {
      return Result<void>::failure(failure("asset: invalid or duplicate media reference"));
    }
  }
  for (const auto& t : p.tracks) {
    if (t.id.empty() || !tracks.insert(t.id).second || t.clips.size() > 10000) {
      return Result<void>::failure(failure("track: invalid ID or clip limit"));
    }
    std::vector<const EditorClip*> ordered;
    for (const auto& c : t.clips) {
      const auto* a = asset_for(p, c.asset_id);
      if (!a || c.id.empty() || !clips.insert(c.id).second || c.position < 0 || c.source_in < 0 ||
          c.duration <= 0 || c.duration > kMaxFrames || c.position > kMaxFrames - c.duration ||
          c.source_in > kMaxFrames - c.duration || !finite(c.x, -4, 4) || !finite(c.y, -4, 4) ||
          !finite(c.scale, .01, 8) || !finite(c.opacity, 0, 1) || !finite(c.volume, 0, 4) ||
          (t.audio_only && !a->audio) ||
          (!a->image && editor_time_us(c.source_in + c.duration, p.rate) >
                            a->duration_us + editor_time_us(1, p.rate))) {
        return Result<void>::failure(
            failure("clip " + c.id + ": invalid range or properties", ErrorCode::invalid_argument));
      }
      ordered.push_back(&c);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](auto a, auto b) { return a->position < b->position; });
    for (size_t i = 1; i < ordered.size(); ++i) {
      if (ordered[i]->position < ordered[i - 1]->position + ordered[i - 1]->duration) {
        return Result<void>::failure(
            failure("track " + t.id + ": clips overlap", ErrorCode::invalid_argument));
      }
    }
  }
  return Result<void>::success();
}

namespace editor_detail {
Result<void> atomic_replace(const std::filesystem::path& from, const std::filesystem::path& to) {
#ifdef _WIN32
  if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return Result<void>::failure(
        failure("save: atomic replace failed: " + std::to_string(GetLastError())));
  }
#else
  std::error_code ec;
  std::filesystem::rename(from, to, ec);
  if (ec) {
    return Result<void>::failure(failure("save: " + ec.message()));
  }
#endif
  return Result<void>::success();
}
}  // namespace editor_detail

Result<void> editor_save(const std::filesystem::path& path, const EditorProject& p) {
  auto valid = editor_validate(p);
  if (!valid) {
    return valid;
  }
  Json root = {{"version", 1},
               {"name", p.name},
               {"width", p.width},
               {"height", p.height},
               {"fps_num", p.rate.numerator},
               {"fps_den", p.rate.denominator},
               {"assets", Json::array()},
               {"tracks", Json::array()}};
  std::error_code ec;
  const auto parent = std::filesystem::absolute(path, ec).parent_path();
  if (ec) {
    return Result<void>::failure(failure("save: " + ec.message()));
  }
  for (const auto& a : p.assets) {
    auto source = std::filesystem::u8path(a.path);
    auto relative = source.lexically_relative(parent);
    root["assets"].push_back({{"id", a.id},
                              {"name", a.name},
                              {"path", relative.empty() ? source.u8string() : relative.u8string()},
                              {"duration_us", a.duration_us},
                              {"width", a.width},
                              {"height", a.height},
                              {"video", a.video},
                              {"audio", a.audio},
                              {"image", a.image}});
  }
  for (const auto& t : p.tracks) {
    Json track = {{"id", t.id},
                  {"name", t.name},
                  {"audio_only", t.audio_only},
                  {"locked", t.locked},
                  {"hidden", t.hidden},
                  {"muted", t.muted},
                  {"clips", Json::array()}};
    for (const auto& c : t.clips) {
      track["clips"].push_back({{"id", c.id},
                                {"asset_id", c.asset_id},
                                {"position", c.position},
                                {"source_in", c.source_in},
                                {"duration", c.duration},
                                {"x", c.x},
                                {"y", c.y},
                                {"scale", c.scale},
                                {"opacity", c.opacity},
                                {"volume", c.volume}});
    }
    root["tracks"].push_back(std::move(track));
  }
  auto temp = path;
  temp += "." + std::to_string(next_failure_id()) + ".tmp";
  const auto text = root.dump(2, ' ', false, Json::error_handler_t::replace);
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.flush();
    if (!file) {
      file.close();
      std::filesystem::remove(temp, ec);
      return Result<void>::failure(failure("save: cannot write project"));
    }
  }
  auto result = editor_detail::atomic_replace(temp, path);
  if (!result) {
    std::filesystem::remove(temp, ec);
  }
  return result;
}

Result<EditorProject> editor_load(const std::filesystem::path& path) {
  auto bad = [] {
    return Result<EditorProject>::failure(failure("load: invalid project JSON/version"));
  };
  std::error_code ec;
  if (std::filesystem::file_size(path, ec) > 16 * 1024 * 1024 || ec) {
    return Result<EditorProject>::failure(failure("load: missing project or exceeds 16 MB"));
  }
  std::ifstream file(path, std::ios::binary);
  const auto j = Json::parse(file, nullptr, false);
  if (j.is_discarded() || !fields(j, "name", "version width height fps_num fps_den", "") ||
      j["version"] != 1 || !j.contains("assets") || !j["assets"].is_array() ||
      !j.contains("tracks") || !j["tracks"].is_array() || !integer(j["width"], 2, 4096) ||
      !integer(j["height"], 2, 4096) || !integer(j["fps_num"], 1, 240000) ||
      !integer(j["fps_den"], 1, 1001000)) {
    return bad();
  }
  EditorProject p;
  p.name = j["name"].get<std::string>();
  p.width = j["width"].get<int>();
  p.height = j["height"].get<int>();
  p.rate = {j["fps_num"].get<int>(), j["fps_den"].get<int>()};
  auto parent = std::filesystem::absolute(path, ec).parent_path();
  for (const auto& a : j["assets"]) {
    if (!fields(a, "id name path", "duration_us width height", "video audio image") ||
        !integer(a["duration_us"], 0, INT64_C(100000000000000)) || !integer(a["width"], 0, 16384) ||
        !integer(a["height"], 0, 16384)) {
      return bad();
    }
    auto input = std::filesystem::u8path(a["path"].get<std::string>());
    p.assets.push_back(
        {a["id"], a["name"],
         (input.is_absolute() ? input : parent / input).lexically_normal().u8string(),
         a["duration_us"], a["width"], a["height"], a["video"], a["audio"], a["image"]});
  }
  for (const auto& t : j["tracks"]) {
    if (!fields(t, "id name", "", "audio_only locked hidden muted") || !t.contains("clips") ||
        !t["clips"].is_array()) {
      return bad();
    }
    EditorTrack track{t["id"],    t["name"], t["audio_only"], t["locked"], t["hidden"],
                      t["muted"], {}};
    for (const auto& c : t["clips"]) {
      if (!fields(c, "id asset_id", "position source_in duration x y scale opacity volume", "") ||
          !integer(c["position"], 0, kMaxFrames) || !integer(c["source_in"], 0, kMaxFrames) ||
          !integer(c["duration"], 1, kMaxFrames)) {
        return bad();
      }
      track.clips.push_back({c["id"], c["asset_id"], c["position"], c["source_in"], c["duration"],
                             c["x"], c["y"], c["scale"], c["opacity"], c["volume"]});
    }
    p.tracks.push_back(std::move(track));
  }
  auto valid = editor_validate(p);
  return valid ? Result<EditorProject>::success(std::move(p))
               : Result<EditorProject>::failure(valid.failure_record());
}

const EditorProject& EditorDocument::project() const {
  return project_;
}

Result<void> EditorDocument::replace(EditorProject project) {
  auto valid = editor_validate(project);
  if (!valid) {
    return valid;
  }
  project_ = std::move(project);
  undo_.clear();
  redo_.clear();
  undo_ids_.clear();
  redo_ids_.clear();
  current_id_ = saved_id_ = ++next_id_;
  return Result<void>::success();
}

Result<void> EditorDocument::edit(EditorProject project) {
  auto valid = editor_validate(project);
  if (!valid) {
    return valid;
  }
  // Lock protects track contents, not the lock toggle itself or asset relinking.
  for (const auto& old : project_.tracks) {
    if (!old.locked) {
      continue;
    }
    const auto found = std::find_if(project.tracks.begin(), project.tracks.end(),
                                    [&](const auto& t) { return t.id == old.id; });
    if (found == project.tracks.end()) {
      return Result<void>::failure(failure("edit: track is locked"));
    }
    auto same_clip = [](const EditorClip& a, const EditorClip& b) {
      return a.id == b.id && a.asset_id == b.asset_id && a.position == b.position &&
             a.source_in == b.source_in && a.duration == b.duration && a.x == b.x && a.y == b.y &&
             a.scale == b.scale && a.opacity == b.opacity && a.volume == b.volume;
    };
    if (old.clips.size() != found->clips.size() ||
        !std::equal(old.clips.begin(), old.clips.end(), found->clips.begin(), same_clip)) {
      return Result<void>::failure(failure("edit: track is locked"));
    }
  }
  undo_.push_back(std::move(project_));
  undo_ids_.push_back(current_id_);
  if (undo_.size() > 100) {
    undo_.erase(undo_.begin());
    undo_ids_.erase(undo_ids_.begin());
  }
  redo_.clear();
  redo_ids_.clear();
  project_ = std::move(project);
  current_id_ = ++next_id_;
  return Result<void>::success();
}

Result<void> EditorDocument::split(const std::string& id, int64_t frame, std::string new_id) {
  auto p = project_;
  for (auto& t : p.tracks) {
    for (auto& c : t.clips) {
      if (c.id != id) {
        continue;
      }
      const auto offset = frame - c.position;
      if (offset <= 0 || offset >= c.duration) {
        return Result<void>::failure(failure("split: playhead must be inside clip"));
      }
      auto right = c;
      right.id = std::move(new_id);
      right.position = frame;
      right.source_in += offset;
      right.duration -= offset;
      c.duration = offset;
      t.clips.push_back(std::move(right));
      return edit(std::move(p));
    }
  }
  return Result<void>::failure(failure("split: clip not found"));
}

Result<void> EditorDocument::move(const std::string& id, const std::string& track_id,
                                  int64_t frame) {
  auto p = project_;
  std::optional<EditorClip> clip;
  for (auto& t : p.tracks) {
    for (auto it = t.clips.begin(); it != t.clips.end(); ++it) {
      if (it->id == id) {
        clip = *it;
        t.clips.erase(it);
        break;
      }
    }
  }
  if (clip) {
    for (auto& t : p.tracks) {
      if (t.id == track_id) {
        clip->position = frame;
        t.clips.push_back(*clip);
        return edit(std::move(p));
      }
    }
  }
  return Result<void>::failure(failure("move: clip or destination not found"));
}

Result<void> EditorDocument::trim(const std::string& id, int64_t source_in, int64_t duration) {
  auto p = project_;
  for (auto& t : p.tracks) {
    for (auto& c : t.clips) {
      if (c.id == id) {
        c.source_in = source_in;
        c.duration = duration;
        return edit(std::move(p));
      }
    }
  }
  return Result<void>::failure(failure("trim: clip not found"));
}

bool EditorDocument::undo() {
  if (undo_.empty()) {
    return false;
  }
  redo_.push_back(std::move(project_));
  redo_ids_.push_back(current_id_);
  project_ = std::move(undo_.back());
  current_id_ = undo_ids_.back();
  undo_.pop_back();
  undo_ids_.pop_back();
  return true;
}

bool EditorDocument::redo() {
  if (redo_.empty()) {
    return false;
  }
  undo_.push_back(std::move(project_));
  undo_ids_.push_back(current_id_);
  project_ = std::move(redo_.back());
  current_id_ = redo_ids_.back();
  redo_.pop_back();
  redo_ids_.pop_back();
  return true;
}

bool EditorDocument::can_undo() const {
  return !undo_.empty();
}

bool EditorDocument::can_redo() const {
  return !redo_.empty();
}

bool EditorDocument::dirty() const {
  return saved_id_ != current_id_;
}

void EditorDocument::mark_saved() {
  saved_id_ = current_id_;
}
}  // namespace livekit::sdk
