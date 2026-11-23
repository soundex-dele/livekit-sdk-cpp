#include "backends/ffmpeg/ffmpeg_playback.hpp"
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <limits>
#include "backends/ffmpeg/windows_file_read.hpp"

namespace livekit::sdk::detail {
struct DemuxerNode::PlaybackFile {
#ifdef _WIN32
  HANDLE handle = INVALID_HANDLE_VALUE, event = nullptr;
  int64_t position = 0;

  ~PlaybackFile() {
    if (event)
      CloseHandle(event);
    if (handle != INVALID_HANDLE_VALUE)
      CloseHandle(handle);
  }
#else
  FILE* stream = nullptr;

  ~PlaybackFile() {
    if (stream)
      std::fclose(stream);
  }
#endif
};

#ifdef _WIN32
static Failure file_failure(const char* operation, DWORD error) {
  return make_failure(ErrorDomain::media, ErrorCode::processing_failed,
                      std::string("playback/file.demuxer/io: ") + operation + " failed", false,
                      static_cast<int>(error), {operation, {}, "file.demuxer"});
}
#endif

DemuxerNode::DemuxerNode(std::atomic<bool>& cancel, std::chrono::milliseconds timeout)
    : MediaNode("file.demuxer"), cancel_(cancel), timeout_(timeout) {}

DemuxerNode::~DemuxerNode() {
  doRelease();
}

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

Result<void> DemuxerNode::begin_io() {
  deadline_ = std::chrono::steady_clock::now() + timeout_;
  io_error_.reset();
  return check_cancelled();
}

int DemuxerNode::interrupt(void* self) {
  auto& node = *static_cast<DemuxerNode*>(self);
  if (auto result = node.check_cancelled(); !result) {
    node.io_error_ = result.failure_record();
    return 1;
  }
  return 0;
}

Result<void> DemuxerNode::check_cancelled() const {
  if (cancel_.load()) {
    return Result<void>::failure(
        playback_failure("playback/file.demuxer/io: cancelled", ErrorCode::cancelled));
  }
  if (std::chrono::steady_clock::now() >= deadline_) {
    return Result<void>::failure(
        playback_failure("playback/file.demuxer/io: timed out", ErrorCode::timeout));
  }
  return Result<void>::success();
}

Result<void> DemuxerNode::doInit() {
  if (auto result = check_ffmpeg_abi(); !result) {
    return Result<void>::failure(result.failure_record());
  }
  if (auto result = begin_io(); !result) {
    return Result<void>::failure(result.failure_record());
  }
  if (path_.empty() || path_.find('\0') != std::string::npos ||
      path_.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return Result<void>::failure(playback_failure("playback/file.demuxer/io: invalid local path",
                                                  ErrorCode::invalid_argument));
  }
#ifdef _WIN32
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path_.data(),
                                         static_cast<int>(path_.size()), nullptr, 0);
  if (!length) {
    return Result<void>::failure(playback_failure("playback/file.demuxer/io: invalid UTF-8 path",
                                                  ErrorCode::invalid_argument));
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
    return Result<void>::failure(
        playback_failure("playback/file.demuxer/io: expected a local regular file"));
  }
  auto file = std::make_unique<PlaybackFile>();
#ifdef _WIN32
  file->handle = CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
  if (file->handle == INVALID_HANDLE_VALUE)
    return Result<void>::failure(file_failure("open", GetLastError()));
  if (GetFileType(file->handle) != FILE_TYPE_DISK)
    return Result<void>::failure(
        playback_failure("playback/file.demuxer/io: expected a local regular file"));
  LARGE_INTEGER bytes;
  if (!GetFileSizeEx(file->handle, &bytes))
    return Result<void>::failure(file_failure("size", GetLastError()));
  size_ = bytes.QuadPart;
  file->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!file->event)
    return Result<void>::failure(file_failure("read event", GetLastError()));
#else
  const auto bytes = std::filesystem::file_size(path, filesystem_error);
  if (filesystem_error || bytes > static_cast<uintmax_t>(std::numeric_limits<int64_t>::max()))
    return Result<void>::failure(playback_failure("playback/file.demuxer/io: invalid file size"));
  size_ = static_cast<int64_t>(bytes);
  file->stream = std::fopen(path.c_str(), "rb");
  if (!file->stream)
    return Result<void>::failure(playback_failure("playback/file.demuxer/io: cannot open file"));
#endif
  file_ = std::move(file);
  return Result<void>::success();
}

// FFmpeg calls back synchronously into this graph node; no intermediate byte event.
int DemuxerNode::read_bytes(void* self, uint8_t* buffer, int size) {
  auto& node = *static_cast<DemuxerNode*>(self);
  if (interrupt(self)) {
    return AVERROR_EXIT;
  }
  if (size <= 0) {
    return AVERROR(EINVAL);
  }
#ifdef _WIN32
  auto& file = *node.file_;
  const auto read = playback_read_at(file.handle, file.event, file.position, buffer,
                                     std::min(size, 32768), [&] { return interrupt(self) != 0; });
  if (read.interrupted || interrupt(self))
    return AVERROR_EXIT;
  if (read.error == ERROR_HANDLE_EOF)
    return AVERROR_EOF;
  if (read.error != ERROR_SUCCESS) {
    node.io_error_ = file_failure("read", read.error);
    return AVERROR(EIO);
  }
  file.position += read.bytes;
  return read.bytes ? static_cast<int>(read.bytes) : AVERROR_EOF;
#else
  const auto count = std::fread(buffer, 1, std::min(size, 32768), node.file_->stream);
  if (interrupt(self)) {
    return AVERROR_EXIT;
  }
  if (!count) {
    return std::ferror(node.file_->stream) ? AVERROR(EIO) : AVERROR_EOF;
  }
  return static_cast<int>(count);
#endif
}

int64_t DemuxerNode::seek_bytes(void* self, int64_t offset, int whence) {
  auto& node = *static_cast<DemuxerNode*>(self);
  if (interrupt(self)) {
    return AVERROR_EXIT;
  }
  if (whence & AVSEEK_SIZE) {
    return node.size_;
  }
  whence &= ~AVSEEK_FORCE;
  if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
    return AVERROR(EINVAL);
  }
#ifdef _WIN32
  const int64_t base = whence == SEEK_SET   ? 0
                       : whence == SEEK_CUR ? node.file_->position
                                            : node.size_;
  if (offset < -base || offset > std::numeric_limits<int64_t>::max() - base)
    return AVERROR(EINVAL);
  return node.file_->position = base + offset;
#else
  return fseeko(node.file_->stream, offset, whence) == 0 ? ftello(node.file_->stream)
                                                         : AVERROR(EIO);
#endif
}

void DemuxerNode::doRelease() {
  pending_.reset();
  av_packet_free(&packet_);
  avformat_close_input(&format_);
  if (io_) {
    av_freep(&io_->buffer);
    avio_context_free(&io_);
  }
  file_.reset();
  video_ = audio_ = -1;
}

}  // namespace livekit::sdk::detail
