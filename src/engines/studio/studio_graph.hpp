#pragma once
#include "internal.hpp"
#include <functional>
namespace graphflow { enum class GraphState; }

namespace livekit::sdk::studio_detail {
// The public Studio facade and the graph worker exchange only this locked state.
// Capture, D3D11 and graph lifecycle operations stay on the worker's COM apartment.
struct StudioGraphState {
  explicit StudioGraphState(StudioOptions);
  StudioOptions options;
  mutable std::mutex mutex;
  StudioScene scene;
  StudioAudioSettings settings;
  StudioSnapshot status;
  std::map<std::string, std::chrono::steady_clock::time_point> remoteSeen;
  std::map<std::string, std::shared_ptr<const StudioImage>> remoteVideo;
  std::map<std::string, std::deque<float>> remoteAudio;
  std::shared_ptr<I420VideoSource> programVideo, localVideo;
  std::shared_ptr<PcmAudioSource> programAudio, localAudio;
  // Includes asynchronous drain/remux; no new session may reuse its path yet.
  bool recordWanted = false, recordBusy = false;
  uint64_t finalizedRecordEvents = 0;
  // Internal fault-injection seam, copied once and run on the encoder mailbox.
  std::function<void()> beforeRecordWrite;
  // Optional test barrier on the control owner, before opening a requested file.
  std::function<void()> beforeRecordOpen;
  // Optional internal observers, configured before start, called on their respective owners;
  // normal Studio does not sample these clocks or retain trace data.
  using Time = std::chrono::steady_clock::time_point;
  std::function<void(Time, Time, Time, Time, AudioWriteStatus)> traceProgramWrite;
  std::function<void(Time, Time)> traceVideoDispatch;
  std::function<void(Time, Time)> traceRecordReady;
  std::function<void(Time, size_t, const std::vector<float>&, const std::vector<float>&)> traceMix;
  std::function<void(Time, const std::vector<float>&)> traceEncoderAudio;
  std::filesystem::path recordPath;
};

// A fixed topology with dynamic scene inputs. Synchronous, typed edge delivery
// runs on separate audio and video owners; rendering cannot stall the PCM clock.
// The recording branch owns a separate bounded GraphFlow mailbox.
class StudioGraph {
 public:
  explicit StudioGraph(StudioGraphState&);
  ~StudioGraph();
  StudioGraph(const StudioGraph&) = delete;
  StudioGraph& operator=(const StudioGraph&) = delete;
  Result<void> start();
  Result<void> step(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  void stop();
  void release();
  Result<graphflow::GraphState> state() const;

  struct NodeInfo {
    std::string name;
    uint64_t received, published;
  };

  struct Connection {
    std::string from, to, type;
  };

  std::vector<NodeInfo> nodes() const;
  std::vector<Connection> connections() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace livekit::sdk::studio_detail
