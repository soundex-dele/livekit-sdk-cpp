#include "backends/ffmpeg/ffmpeg_playback.hpp"
#include <cerrno>
#include <filesystem>
#include <limits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace livekit::sdk::detail {
Result<void> ffmpeg_check(int code, const std::string& operation) {
  if (code >= 0) {
    return Result<void>::success();
  }
  char message[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(code, message, sizeof(message));
  return Result<void>::failure(playback_failure("playback/" + operation + ": " + message));
}

Result<void> check_ffmpeg_abi() {
  if ((avformat_version() >> 16) != LIBAVFORMAT_VERSION_MAJOR ||
      (avcodec_version() >> 16) != LIBAVCODEC_VERSION_MAJOR ||
      (avutil_version() >> 16) != LIBAVUTIL_VERSION_MAJOR ||
      (swscale_version() >> 16) != LIBSWSCALE_VERSION_MAJOR ||
      (swresample_version() >> 16) != LIBSWRESAMPLE_VERSION_MAJOR) {
    return Result<void>::failure(
        playback_failure("playback: FFmpeg runtime and header ABI versions differ"));
  }
  return Result<void>::success();
}

FileSourceNode::FileSourceNode(graphflow::core::EventBus* bus, std::atomic<bool>& cancel,
                               std::chrono::milliseconds timeout)
    : MediaNode("file.source", bus), cancel_(cancel), timeout_(timeout) {}

FileSourceNode::~FileSourceNode() {
  doRelease();
}

void FileSourceNode::begin_io() {
  deadline_ = std::chrono::steady_clock::now() + timeout_;
}

int FileSourceNode::interrupt(void* self) {
  const auto& node = *static_cast<FileSourceNode*>(self);
  return node.cancel_.load() || std::chrono::steady_clock::now() >= node.deadline_;
}

Result<void> FileSourceNode::check_cancelled() const {
  if (cancel_.load()) {
    return Result<void>::failure(
        playback_failure("playback/file.source: cancelled", ErrorCode::cancelled));
  }
  if (std::chrono::steady_clock::now() >= deadline_) {
    return Result<void>::failure(
        playback_failure("playback/file.source: timed out", ErrorCode::timeout));
  }
  return Result<void>::success();
}

bool FileSourceNode::doInit() {
  if (auto result = check_ffmpeg_abi(); !result) {
    reportError(graph_error(result.failure_record()));
    return false;
  }
  begin_io();
  if (auto result = check_cancelled(); !result) {
    reportError(graph_error(result.failure_record()));
    return false;
  }
  if (path_.empty() || path_.find('\0') != std::string::npos ||
      path_.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    reportError(graph_error(
        playback_failure("playback/file.source: invalid local path", ErrorCode::invalid_argument)));
    return false;
  }
#ifdef _WIN32
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path_.data(),
                                         static_cast<int>(path_.size()), nullptr, 0);
  if (!length) {
    reportError(graph_error(
        playback_failure("playback/file.source: invalid UTF-8 path", ErrorCode::invalid_argument)));
    return false;
  }
  std::wstring wide(length, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path_.data(), static_cast<int>(path_.size()),
                      wide.data(), length);
  const std::filesystem::path path(wide);
#else
  const std::filesystem::path path(path_);
#endif
  std::error_code filesystem_error;
  if (!std::filesystem::is_regular_file(path, filesystem_error)) {
    reportError(
        graph_error(playback_failure("playback/file.source: expected a local regular file")));
    return false;
  }
  const auto bytes = std::filesystem::file_size(path, filesystem_error);
  if (filesystem_error) {
    reportError(graph_error(playback_failure("playback/file.source: cannot get file size")));
    return false;
  }
  if (bytes > static_cast<uintmax_t>(std::numeric_limits<int64_t>::max())) {
    reportError(graph_error(playback_failure("playback/file.source: file too large")));
    return false;
  }
  size_ = static_cast<int64_t>(bytes);
#ifdef _WIN32
  file_ = _wfopen(path.c_str(), L"rb");
#else
  file_ = std::fopen(path.c_str(), "rb");
#endif
  if (!file_) {
    reportError(graph_error(playback_failure("playback/file.source: cannot open file")));
    return false;
  }
  m_eventBus->subscribe<FileIORequest>(getName() + ".io", [this](FileIORequest& request) {
    request.handled = true;
    auto result = handle_io(request);
    if (!result) {
      request.error = result.failure_record();
    }
  });
  finished_ = true;  // Demand-driven byte service; stream EOS belongs to demux.
  return true;
}

Result<void> FileSourceNode::handle_io(FileIORequest& request) {
  if (request.kind == FileIORequest::begin) {
    begin_io();
    {
      auto checked = check_cancelled();
      if (!checked) {
        return Result<void>::failure(checked.failure_record());
      }
    }
    return Result<void>::success();
  }
  {
    auto checked = check_cancelled();
    if (!checked) {
      return Result<void>::failure(checked.failure_record());
    }
  }
  if (request.kind == FileIORequest::read) {
    if (request.size <= 0) {
      return Result<void>::failure(playback_failure("playback/file.source: invalid read size"));
    }
    auto bytes = std::make_shared<std::vector<uint8_t>>(std::min(request.size, 32768));
    const auto count = std::fread(bytes->data(), 1, bytes->size(), file_);
    {
      auto checked = check_cancelled();
      if (!checked) {
        return Result<void>::failure(checked.failure_record());
      }
    }
    if (!count) {
      request.result = std::ferror(file_) ? AVERROR(EIO) : AVERROR_EOF;
      return Result<void>::success();
    }
    bytes->resize(count);
    SeekableBytes event;
    event.bytes = std::move(bytes);
    auto published = publish_media(std::move(event));
    if (!published) {
      return Result<void>::failure(published.failure_record());
    }
    if (!*published) {
      return Result<void>::failure(
          playback_failure("playback/file.source: bytes were not consumed"));
    }
    request.result = static_cast<int64_t>(count);
    return Result<void>::success();
  }
  if (request.whence & AVSEEK_SIZE) {
    request.result = size_;
    return Result<void>::success();
  }
  const int whence = request.whence & ~AVSEEK_FORCE;
  if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
    request.result = AVERROR(EINVAL);
    return Result<void>::success();
  }
#ifdef _WIN32
  request.result = _fseeki64(file_, request.offset, whence) == 0 ? _ftelli64(file_) : AVERROR(EIO);
#else
  request.result = fseeko(file_, request.offset, whence) == 0 ? ftello(file_) : AVERROR(EIO);
#endif
  return Result<void>::success();
}

void FileSourceNode::doRelease() {
  m_eventBus->unsubscribe(getName() + ".io");
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
}
}  // namespace livekit::sdk::detail
