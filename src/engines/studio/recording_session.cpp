#include "recording_session.hpp"
#include <graphflow/graph.hpp>
#include <condition_variable>
#include <thread>

namespace livekit::sdk::studio_detail {
namespace gf = graphflow;

namespace {
constexpr size_t kVideoWaiting = 6, kTotalWaiting = 256;
using Time = RecordingSession::Time;

struct Video {
  std::shared_ptr<const StudioImage> frame;
  Time time;
};

struct Audio {
  std::vector<float> samples;
  Time time;
};
}  // namespace

struct RecordingSession::Impl {
  mutable std::mutex mutex;
  std::condition_variable wake;
  bool ready = false, closing = false;
  Status result;
  gf::Emitter<Audio> audio;
  gf::Emitter<Video> video;
  std::function<void()> beforeWrite;
  AudioTrace traceAudio;
  std::thread owner;

  Impl(std::filesystem::path path, StudioOptions options, std::function<Time()> origin,
       std::function<void()> hook, AudioTrace trace)
      : beforeWrite(std::move(hook)),
        traceAudio(std::move(trace)),
        owner([this, path = std::move(path), options, origin = std::move(origin)] {
          run(path, options, origin);
          std::lock_guard lock(mutex);
          ready = result.done = true;
          wake.notify_all();
        }) {
    std::unique_lock lock(mutex);
    wake.wait(lock, [this] { return ready; });
  }

  void failLocked(Failure f) {
    if (!result.first) {
      result.first = std::move(f);
    } else if (result.first != f && !result.cleanup) {
      result.cleanup = std::move(f);
    }
    closing = true;
    wake.notify_one();
  }

  void observe(Result<void> status) {
    if (!status) {
      std::lock_guard lock(mutex);
      failLocked(status.failure_record());
    }
  }

  gf::Status written(Result<void> s) {
    if (s) {
      return {};
    }
    observe(s);  // Wake the owner even if there are no more Studio ticks.
    return {{gf::ErrorCode::NodeFailure, s.error().message, s.failure_record()}};
  }

  // Both ports share one receiver mailbox. A failed write shuts admission for
  // both streams. Admission failure alone must NOT skip already accepted writes.
  struct Sink : public gf::INode {
    Impl& session;
    Recorder& encoder;

    Sink(Impl& s, Recorder& r) : session(s), encoder(r) {}

    void bind(gf::Binder& b) override {
      b.input(gf::In<Audio>{"audio", gf::Connections::ExactlyOne}, [this](const Audio& a) {
        if (session.beforeWrite) {
          session.beforeWrite();
        }
        if (session.traceAudio) {
          session.traceAudio(a.time, a.samples);
        }
        return session.written(encoder.audio(a.samples, a.time));
      });
      b.input(gf::In<Video>{"video", gf::Connections::ExactlyOne}, [this](const Video& v) {
        if (session.beforeWrite) {
          session.beforeWrite();
        }
        return session.written(encoder.video(*v.frame, v.time));
      });
    }
  };

  bool check(const gf::Status& s) {
    if (!s) {
      // Our sink is the only producer of application detail in this private graph.
      if (s.error.detail) {
        observe(failed(std::static_pointer_cast<const FailureInfo>(s.error.detail)));
      } else {
        observe(failed("Studio recording graph: " + s.error.message));
      }
    }
    return bool(s);
  }

  void run(const std::filesystem::path& path, const StudioOptions& o, const std::function<Time()>& origin) {
    Recorder encoder(path, o.width, o.height, o.fps);
    auto opened = encoder.status();
    if (!opened) {
      observe(opened);
      return;
    }
    gf::Graph graph({gf::Dispatch::Queued, kTotalWaiting});
    auto a = graph.ingress<Audio>("studio.record-audio");
    auto v = graph.ingress<Video>("studio.record-video");
    auto sink = graph.add<Sink>("studio.file-encoder", *this, encoder);
    if (check(a.status()) && check(v.status()) && check(sink.status()) &&
        check(graph.rewire([&](auto& w) {
          w.connect(a->port, sink->in(gf::In<Audio>{"audio"}), {},
                    {gf::DeliveryPolicy::Reliable,
                     kTotalWaiting,
                     {kTotalWaiting * 960 * sizeof(float), 0}});
          w.connect(v->port, sink->in(gf::In<Video>{"video"}), {},
                    {gf::DeliveryPolicy::Reliable,
                     kVideoWaiting,
                     {kVideoWaiting * uint64_t(o.width) * o.height * 4, 0}});
        })) &&
        check(graph.start())) {
      {
        std::unique_lock lock(mutex);
        audio = a->writer;
        video = v->writer;
        // One epoch for both producers, independent of encoder startup cost and arrival order.
        result.mediaOrigin = origin();
        encoder.setMediaOrigin(*result.mediaOrigin);
        ready = true;
        wake.notify_all();
        wake.wait(lock, [this] { return closing; });
      }
      check(graph.stop());  // Drain every accepted event before flushing FFmpeg.
    }
    check(graph.close());
    observe(encoder.finish());
  }

  template <class F>
  void admit(F send) {
    std::lock_guard lock(mutex);
    if (closing) {
      return;
    }
    const auto r = send();
    if (r.status == gf::Admission::Accepted && r.accepted == 1) {
      ++result.admitted;
      return;
    }
    const bool full = r.status == gf::Admission::Full || r.status == gf::Admission::TooLarge;
    failLocked(failed(full ? "Recording backlog limit reached; finalizing accepted media"
                           : "Recording graph rejected media: " + r.error.message,
                      full ? ErrorCode::resource_exhausted : ErrorCode::processing_failed)
                   .error);
  }
};

RecordingSession::RecordingSession(std::filesystem::path p, const StudioOptions& o,
                                   std::function<Time()> origin, std::function<void()> hook,
                                   AudioTrace trace)
    : impl_(std::make_unique<Impl>(std::move(p), o, std::move(origin), std::move(hook),
                                   std::move(trace))) {}

RecordingSession::~RecordingSession() {
  wait();
}

void RecordingSession::stop() {
  std::lock_guard lock(impl_->mutex);
  impl_->closing = true;
  impl_->wake.notify_all();
}

void RecordingSession::wait() {
  stop();
  if (impl_->owner.joinable()) {
    impl_->owner.join();
  }
}

RecordingSession::Status RecordingSession::status() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->result;
}

void RecordingSession::video(std::shared_ptr<const StudioImage> image, Time time) {
  impl_->admit([&] {
    const gf::QueueCost cost{image->rgba.size(), 0};
    return impl_->video.emit(Video{std::move(image), time}, {}, cost);
  });
}

void RecordingSession::audio(const std::vector<float>& samples, Time time) {
  impl_->admit([&] {
    return impl_->audio.emit(Audio{samples, time}, {},
                             gf::QueueCost{samples.size() * sizeof(float), 0});
  });
}
}  // namespace livekit::sdk::studio_detail
