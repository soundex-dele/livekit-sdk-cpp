#pragma once
#include "internal.hpp"
#include "core/lifecycle.h"

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
 std::map<std::string,std::chrono::steady_clock::time_point> remoteSeen;
 std::map<std::string,std::shared_ptr<const StudioImage>> remoteVideo;
 std::map<std::string,std::deque<float>> remoteAudio;
 std::shared_ptr<I420VideoSource> programVideo,localVideo;
 std::shared_ptr<PcmAudioSource> programAudio,localAudio;
 bool recordWanted=false;
 std::filesystem::path recordPath;
};

// A fixed topology with dynamic scene inputs. Synchronous, typed edge delivery
// keeps the 10 ms audio clock and 30 fps video clock on one owner sequence.
// Device capture and Recorder retain their bounded worker queues.
class StudioGraph {
 public:
 explicit StudioGraph(StudioGraphState&);
 ~StudioGraph();
 StudioGraph(const StudioGraph&)=delete;
 StudioGraph& operator=(const StudioGraph&)=delete;
 void start();
 void step(std::chrono::steady_clock::time_point now=std::chrono::steady_clock::now());
 void stop();
 void release();
 struct NodeInfo {std::string name;graphflow::core::LifecycleState state;uint64_t received,published;};
 struct Connection {std::string from,to,type;};
 std::vector<NodeInfo> nodes() const;
 std::vector<Connection> connections() const;
 private:
 struct Impl;
 std::unique_ptr<Impl> impl_;
};
}
