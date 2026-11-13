#include "editor_internal.hpp"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace livekit::sdk {
using editor_detail::failure;

namespace {
thread_local const void* export_worker = nullptr;
}  // namespace

class EditorSession::Impl {
 public:
  explicit Impl(EditorCallbacks callbacks)
      : callbacks(std::move(callbacks)), worker([this] { run(); }) {}

  ~Impl() {
    cancelled = true;
    {
      std::lock_guard lock(mutex);
      stopping = true;
    }
    ready.notify_all();
    worker.join();
  }

  template <class T, class Fn>
  AsyncResult<T> submit(Fn fn, bool seek = false) {
    auto source = std::make_shared<CompletionSource<T>>();
    auto result = source->get_result();
    std::vector<std::function<void()>> superseded;
    {
      std::lock_guard lock(mutex);
      if (seek) {
        for (auto it = commands.begin(); it != commands.end();) {
          if (it->seek) {
            superseded.push_back(std::move(it->run));
            it = commands.erase(it);
          } else {
            ++it;
          }
        }
      }
      if (stopping || commands.size() >= 64) {
        source->set_failure(failure("session: command queue full/closed", ErrorCode::busy));
        return result;
      }
      commands.push_back(
          {[source, fn = std::move(fn)]() mutable { source->set_result(fn()); }, seek});
    }
    ready.notify_one();
    return result;
  }

  Result<void> render(uint64_t seek_token = 0) {
    if (!graph) {
      return Result<void>::failure(failure("preview: no project", ErrorCode::invalid_state));
    }
    auto frame = graph->render(position, generation);
    if (!frame) {
      return Result<void>::failure(frame.failure_record());
    }
    if (seek_token && seek_token != seeks) {
      return Result<void>::failure(failure("seek: superseded", ErrorCode::cancelled));
    }
    if (callbacks.frame) {
      callbacks.frame(*frame);
    }
    return Result<void>::success();
  }

  void run() {
    std::unique_lock lock(mutex);
    for (;;) {
      if (stopping && commands.empty()) {
        break;
      }
      if (!commands.empty()) {
        auto command = std::move(commands.front());
        commands.pop_front();
        lock.unlock();
        command.run();
        lock.lock();
        continue;
      }
      if (!playing) {
        ready.wait(lock, [&] { return stopping || !commands.empty() || playing; });
        continue;
      }
      if (ready.wait_until(lock, deadline, [&] { return stopping || !commands.empty(); })) {
        continue;
      }
      lock.unlock();
      auto result = render();
      if (!result) {
        playing = false;
        if (callbacks.error) {
          callbacks.error(result.failure_record());
        }
      } else if (++position >= editor_duration(project)) {
        playing = false;
        if (callbacks.ended) {
          callbacks.ended();
        }
      } else {
        deadline =
            start + std::chrono::microseconds(editor_time_us(position - start_frame, project.rate));
      }
      lock.lock();
    }
    lock.unlock();
    graph.reset();
  }

  EditorCallbacks callbacks;
  std::atomic<bool> cancelled{false};
  std::atomic<uint64_t> seeks{0};
  std::mutex mutex;
  std::condition_variable ready;

  struct Command {
    std::function<void()> run;
    bool seek = false;
  };

  std::deque<Command> commands;
  bool stopping = false, playing = false;
  EditorProject project;
  std::unique_ptr<editor_detail::RenderGraph> graph;
  int64_t position = 0, start_frame = 0;
  uint64_t generation = 0;
  std::chrono::steady_clock::time_point start, deadline;
  std::thread worker;
};

EditorSession::EditorSession(EditorCallbacks callbacks)
    : impl_(std::make_unique<Impl>(std::move(callbacks))) {}

EditorSession::~EditorSession() = default;

AsyncResult<EditorProbe> EditorSession::probe(std::string path) {
  return impl_->submit<EditorProbe>([p = impl_.get(), path = std::move(path)] {
    return editor_detail::probe(path, p->cancelled);
  });
}

AsyncResult<void> EditorSession::set_project(EditorProject project, uint64_t generation,
                                             int64_t frame, bool play_after_update) {
  return impl_->submit<void>([p = impl_.get(), project = std::move(project), generation, frame,
                              play_after_update]() mutable {
    auto valid = editor_validate(project);
    if (!valid) {
      return valid;
    }
    if (p->graph) {
      auto updated = p->graph->update_project(project);
      if (!updated) {
        p->playing = false;
        return updated;
      }
    } else {
      p->graph = std::make_unique<editor_detail::RenderGraph>(project, p->cancelled);
    }
    p->playing = false;
    p->project = std::move(project);
    p->generation = generation;
    p->position = std::clamp<int64_t>(frame, 0, editor_duration(p->project));
    auto rendered = p->render();
    if (!rendered || !play_after_update) {
      return rendered;
    }
    const auto duration = editor_duration(p->project);
    if (p->position < duration) {
      p->start_frame = p->position;
      p->start = std::chrono::steady_clock::now();
      ++p->position;  // The requested frame has already been delivered.
    }
    p->playing = p->position < duration;
    if (p->playing) {
      p->deadline = p->start + std::chrono::microseconds(
                                   editor_time_us(p->position - p->start_frame, p->project.rate));
    } else if (p->callbacks.ended) {
      p->callbacks.ended();
    }
    return rendered;
  });
}

AsyncResult<void> EditorSession::seek(int64_t frame) {
  const auto serial = ++impl_->seeks;
  return impl_->submit<void>(
      [p = impl_.get(), frame, serial] {
        if (serial != p->seeks) {
          return Result<void>::failure(failure("seek: superseded", ErrorCode::cancelled));
        }
        p->playing = false;
        p->position = std::clamp<int64_t>(frame, 0, editor_duration(p->project));
        return p->render(serial);
      },
      true);
}

AsyncResult<void> EditorSession::play() {
  return impl_->submit<void>([p = impl_.get()] {
    if (!p->graph || editor_duration(p->project) == 0) {
      return Result<void>::failure(failure("play: timeline is empty", ErrorCode::invalid_state));
    }
    if (p->position >= editor_duration(p->project)) {
      p->position = 0;
    }
    p->playing = true;
    p->start_frame = p->position;
    p->start = p->deadline = std::chrono::steady_clock::now();
    return Result<void>::success();
  });
}

AsyncResult<void> EditorSession::pause() {
  return impl_->submit<void>([p = impl_.get()] {
    p->playing = false;
    return Result<void>::success();
  });
}

class EditorExport::Impl {
 public:
  ~Impl() {
    cancel = true;
    if (worker.joinable()) {
      worker.join();
    }
  }

  std::mutex mutex;
  std::atomic<bool> cancel{false}, active{false};
  std::thread worker;
};

EditorExport::EditorExport() : impl_(std::make_unique<Impl>()) {}

EditorExport::~EditorExport() = default;

AsyncResult<void> EditorExport::start(EditorProject project, std::filesystem::path output,
                                      std::function<void(EditorExportProgress)> progress) {
  auto source = std::make_shared<CompletionSource<void>>();
  auto result = source->get_result();
  if (export_worker == impl_.get()) {
    source->set_failure(failure("export: restart from a completion callback requires dispatch",
                                ErrorCode::invalid_thread));
    return result;
  }
  std::lock_guard lock(impl_->mutex);
  if (impl_->active.exchange(true)) {
    source->set_failure(failure("export: already running", ErrorCode::busy));
    return result;
  }
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
  impl_->cancel = false;
  impl_->worker = std::thread([p = impl_.get(), source, project = std::move(project),
                               output = std::move(output), progress = std::move(progress)] {
    export_worker = p;
    auto exported = editor_detail::export_mp4(project, output, p->cancel, progress);
    p->active = false;
    source->set_result(std::move(exported));
    export_worker = nullptr;
  });
  return result;
}

void EditorExport::cancel() {
  impl_->cancel = true;
}
}  // namespace livekit::sdk
