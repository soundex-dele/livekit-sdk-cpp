#include "media_graph/media_flow.hpp"
#include "engine/graph_engine.h"
#include <iostream>

using namespace livekit::sdk::detail;

namespace {
void require(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}

template <class F>
void fails(F f) {
  bool failed = false;
  try {
    f();
  } catch (const std::exception&) {
    failed = true;
  }
  require(failed, "expected failure");
}

class Source : public MediaNode {
 public:
  explicit Source(graphflow::core::EventBus* bus) : MediaNode("producer", bus) {}

  bool pump() override {
    if (finished_)
      return false;
    RGBA8 event;
    event.eos = next == 4;
    event.frame.timestamp_us = next;
    event.frame.rgba = std::make_shared<const std::vector<uint8_t>>(4, static_cast<uint8_t>(next));
    if (!publish_media(event))
      return false;
    finished_ = event.eos;
    ++next;
    return true;
  }

  int next = 0;

 protected:
  void on_reset(int64_t) override {
    next = 0;
  }
};

class Transform : public MediaNode {
 public:
  explicit Transform(graphflow::core::EventBus* bus) : MediaNode("processor", bus) {}

  bool pump() override {
    if (!pending)
      return false;
    if (!publish_media(*pending))
      return false;
    finished_ = pending->eos;
    pending.reset();
    return true;
  }

 protected:
  bool doInit() override {
    subscribe_media<RGBA8>([this](const RGBA8& event) {
      if (pending)
        return false;
      pending = event;
      pending->frame.timestamp_us += 10;
      return true;
    });
    return true;
  }

  void on_reset(int64_t) override {
    pending.reset();
  }

 private:
  std::optional<RGBA8> pending;
};

class Sink : public MediaNode {
 public:
  explicit Sink(graphflow::core::EventBus* bus) : MediaNode("consumer", bus) {}

  bool blocked = true, broken = false;
  std::vector<livekit::sdk::media::VideoFrame> frames;

 protected:
  bool doInit() override {
    subscribe_media<RGBA8>([this](const RGBA8& event) {
      if (broken)
        throw std::runtime_error("sink failure");
      if (blocked)
        return false;
      if (event.eos)
        finished_ = true;
      else
        frames.push_back(event.frame);
      return true;
    });
    return true;
  }

  void on_reset(int64_t) override {
    frames.clear();
  }
};
}  // namespace

int main() {
  try {
    graphflow::engine::GraphEngine engine{1};
    auto source = std::make_shared<Source>(nullptr);
    auto transform = std::make_shared<Transform>(nullptr);
    auto sink = std::make_shared<Sink>(nullptr);
    require(
        engine.loadFromNodes({sink, transform, source}, {{"producer", "processor", "RGBA8", ""},
                                                         {"processor", "consumer", "RGBA8", ""}}),
        "loadFromNodes failed");
    require(
        engine.paths().size() == 1 && engine.paths().front()->getEventBus() == engine.eventBus(),
        "engine did not own graph");
    auto edge_buses = engine.paths().front()->eventBusesSnapshot();
    require(edge_buses.size() == 2, "engine did not retain both edge buses");
    MediaExecutor executor({sink, source, transform});  // intentionally not in execution order
    require(executor.nodes().front() == source && executor.nodes().back() == sink,
            "executor ignored graph connections");
    require(engine.init() && engine.start(), "engine init/start failed");
    require(executor.step() == MediaExecutor::Step::progress, "source did not publish");
    require(executor.step() == MediaExecutor::Step::backpressured,
            "subscriber did not backpressure source");
    require(source->next == 1 && sink->frames.empty(), "blocked subscriber lost or consumed data");
    sink->blocked = false;
    for (int i = 0; i < 20 && executor.step() != MediaExecutor::Step::end; ++i) {
    }
    require(sink->finished() && sink->frames.size() == 4, "EOS overtook data or graph stalled");
    for (int i = 0; i < 4; ++i)
      require(sink->frames[i].timestamp_us == i + 10 && sink->frames[i].rgba->front() == i,
              "subscription processing lost ordering or ownership");
    auto retained = sink->frames.front();
    engine.stop();
    executor.reset(0);
    require(engine.start(), "restart failed");
    sink->blocked = true;
    executor.step();
    engine.stop();
    executor.reset(0);  // discard a blocked in-flight frame
    RGBA8 stale;
    stale.epoch = 1;
    stale.delivery = std::make_shared<MediaDelivery>();
    const auto edge = source->getPubEdges().front();
    edge.eventBus()->publish(edge.name(), stale);
    require(stale.delivery->accepted && !stale.delivery->error, "stale epoch was not discarded");
    sink->blocked = false;
    require(engine.start(), "restart after flush failed");
    for (int i = 0; i < 20 && executor.step() != MediaExecutor::Step::end; ++i) {
    }
    require(sink->frames.size() == 4 && sink->frames.front().timestamp_us == 10,
            "flush retained stale pending media");
    engine.stop();
    executor.reset(0);
    engine.start();
    sink->broken = true;
    executor.step();
    fails([&] { executor.step(); });  // EventBus must not swallow processing errors
    engine.release();
    require(engine.paths().empty() && engine.eventBus()->isClosed(),
            "engine did not release graph");
    for (const auto& edge_bus : edge_buses)
      require(edge_bus->isClosed(), "engine left an edge bus open");
    for (const auto& node : executor.nodes())
      require(node->state() == graphflow::core::LifecycleState::Released,
              "engine left a node alive");
    require(retained.rgba->front() == 0, "release invalidated retained payload");
    fails([&] { executor.step(); });
    std::cout << "PASS: graph-driven subscriptions, ordering, backpressure, EOS, epochs, errors "
                 "and owned payloads\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
