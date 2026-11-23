#include "media_graph/media_flow.hpp"
#include <iostream>
#include <cstdlib>

using namespace livekit::sdk;
using namespace livekit::sdk::detail;

namespace {
void require(bool ok, const char* message) {
  if (!ok) {
    std::cerr << message << "\n";
    std::exit(1);
  }
}

template <class T>
T take(Result<T> result) {
  require(bool(result), result ? "" : result.error().message.c_str());
  if constexpr (!std::is_void_v<T>) {
    return *std::move(result);
  }
}

template <class F>
void fails(F f) {
  require(!f(), "expected failure");
}

class Source : public MediaNode {
 public:
  Source() : MediaNode("producer") {}

  Result<bool> pump() override {
    if (finished_)
      return Result<bool>::success(false);
    RGBA8 event;
    event.eos = next == 4;
    event.frame.timestamp_us = next;
    event.frame.rgba = std::make_shared<const std::vector<uint8_t>>(4, static_cast<uint8_t>(next));
    auto sent = publish_media(output, event);
    if (!sent) {
      return sent;
    }
    if (!*sent)
      return Result<bool>::success(false);
    finished_ = event.eos;
    ++next;
    return Result<bool>::success(true);
  }

  graphflow::Emitter<RGBA8> output;

  void bind(graphflow::Binder& b) override {
    output = b.output(graphflow::Out<RGBA8>{"out"});
  }

  int next = 0;

 protected:
  Result<void> on_reset(int64_t) override {
    next = 0;
    return Result<void>::success();
  }
};

class Transform : public MediaNode {
 public:
  Transform() : MediaNode("processor") {}

  Result<bool> pump() override {
    if (!pending)
      return Result<bool>::success(false);
    auto sent = publish_media(output, *pending);
    if (!sent) {
      return sent;
    }
    if (!*sent)
      return Result<bool>::success(false);
    finished_ = pending->eos;
    pending.reset();
    return Result<bool>::success(true);
  }

 protected:
  void bind(graphflow::Binder& b) override {
    subscribe_media<RGBA8>(b, [this](const RGBA8& event) {
      if (pending)
        return Result<bool>::success(false);
      pending = event;
      pending->frame.timestamp_us += 10;
      return Result<bool>::success(true);
    });
    output = b.output(graphflow::Out<RGBA8>{"out"});
  }

  Result<void> on_reset(int64_t) override {
    pending.reset();
    return Result<void>::success();
  }

 private:
  graphflow::Emitter<RGBA8> output;
  std::optional<RGBA8> pending;
};

class Sink : public MediaNode {
 public:
  Sink() : MediaNode("consumer") {}

  bool blocked = true, broken = false;
  std::vector<livekit::sdk::media::VideoFrame> frames;

 protected:
  void bind(graphflow::Binder& b) override {
    subscribe_media<RGBA8>(b, [this](const RGBA8& event) {
      if (broken)
        return Result<bool>::failure(
            make_failure(ErrorDomain::media, ErrorCode::processing_failed, "sink failure"));
      if (blocked)
        return Result<bool>::success(false);
      if (event.eos)
        finished_ = true;
      else
        frames.push_back(event.frame);
      return Result<bool>::success(true);
    });
  }

  Result<void> on_reset(int64_t) override {
    frames.clear();
    return Result<void>::success();
  }
};
}  // namespace

int main() {
  graphflow::Graph engine({graphflow::Dispatch::Inline});
  auto source = std::make_shared<Source>();
  auto transform = std::make_shared<Transform>();
  auto sink = std::make_shared<Sink>();
  MediaBinding k(engine, sink), s(engine, source), t(engine, transform);
  require(k.ref && s.ref && t.ref, "could not register media nodes");
  std::vector<MediaEdge> edges;
  require(bool(engine.rewire([&](graphflow::Wiring& w) {
            connect_media<RGBA8>(w, edges, s, t);
            connect_media<RGBA8>(w, edges, t, k);
          })),
          "could not wire media graph");
  MediaExecutor executor({sink, source, transform}, edges);  // intentionally unordered
  require(executor.nodes().front() == source && executor.nodes().back() == sink,
          "executor ignored graph connections");
  require(engine.start(graphflow::StartMode::PrepareOnly) && engine.start(),
          "engine init/start failed");
  require(take(executor.step()) == MediaExecutor::Step::progress, "source did not publish");
  require(take(executor.step()) == MediaExecutor::Step::backpressured,
          "subscriber did not backpressure source");
  require(source->next == 1 && sink->frames.empty(), "blocked subscriber lost or consumed data");
  sink->blocked = false;
  for (int i = 0; i < 20 && take(executor.step()) != MediaExecutor::Step::end; ++i) {
  }
  require(sink->finished() && sink->frames.size() == 4, "EOS overtook data or graph stalled");
  for (int i = 0; i < 4; ++i)
    require(sink->frames[i].timestamp_us == i + 10 && sink->frames[i].rgba->front() == i,
            "subscription processing lost ordering or ownership");
  auto retained = sink->frames.front();
  require(bool(engine.stop()), "stop failed");
  take(executor.reset(0));
  require(bool(engine.start()), "restart failed");
  sink->blocked = true;
  take(executor.step());
  require(bool(engine.stop()), "stop failed");
  take(executor.reset(0));  // discard a blocked in-flight frame
  RGBA8 stale;
  stale.epoch = 1;
  stale.delivery = std::make_shared<MediaDelivery>();
  require(bool(engine.start()), "restart after flush failed");
  require(source->output.emit(stale).status == graphflow::Admission::Accepted,
          "stale event not delivered");
  require(stale.delivery->accepted && !stale.delivery->error, "stale epoch was not discarded");
  sink->blocked = false;
  for (int i = 0; i < 20 && take(executor.step()) != MediaExecutor::Step::end; ++i) {
  }
  require(sink->frames.size() == 4 && sink->frames.front().timestamp_us == 10,
          "flush retained stale pending media");
  require(bool(engine.stop()), "stop failed");
  take(executor.reset(0));
  require(bool(engine.start()), "restart failed");
  sink->broken = true;
  take(executor.step());
  auto error = executor.step();
  require(!error && error.error().message == "sink failure", "lost original sink failure");
  require(bool(engine.close()), "close failed");
  require(source->output.emit(stale).status == graphflow::Admission::Closed,
          "released port still accepts media");
  for (const auto& node : executor.nodes())
    require(node->state() == MediaState::Released, "engine left a node alive");
  require(retained.rgba->front() == 0, "release invalidated retained payload");
  fails([&] { return executor.step(); });
  std::cout << "PASS: graph-driven subscriptions, ordering, backpressure, EOS, epochs, errors "
               "and owned payloads\n";
  return 0;
}
