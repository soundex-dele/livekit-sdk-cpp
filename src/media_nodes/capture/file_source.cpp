#include "backends/ffmpeg/ffmpeg_playback.hpp"
#include <cerrno>
#include <filesystem>
#include <limits>

namespace livekit::sdk::detail {
void ffmpeg_check(int code, const std::string& operation) {
  if (code >= 0)
    return;
  char message[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(code, message, sizeof(message));
  throw PlayerError("playback/" + operation + ": " + message);
}

void check_ffmpeg_abi() {
  if ((avformat_version() >> 16) != LIBAVFORMAT_VERSION_MAJOR ||
      (avcodec_version() >> 16) != LIBAVCODEC_VERSION_MAJOR ||
      (avutil_version() >> 16) != LIBAVUTIL_VERSION_MAJOR ||
      (swscale_version() >> 16) != LIBSWSCALE_VERSION_MAJOR ||
      (swresample_version() >> 16) != LIBSWRESAMPLE_VERSION_MAJOR)
    throw PlayerError("playback: FFmpeg runtime and header ABI versions differ");
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

void FileSourceNode::check_cancelled() const {
  if (interrupt(const_cast<FileSourceNode*>(this)))
    throw PlayerError("playback/file.source: cancelled or timed out");
}

bool FileSourceNode::doInit() {
  check_ffmpeg_abi();
  begin_io();
  check_cancelled();
  const auto path = std::filesystem::u8path(path_);
  if (!std::filesystem::is_regular_file(path))
    throw PlayerError("playback/file.source: expected a local regular file");
  const auto bytes = std::filesystem::file_size(path);
  if (bytes > static_cast<uintmax_t>(std::numeric_limits<int64_t>::max()))
    throw PlayerError("playback/file.source: file too large");
  size_ = static_cast<int64_t>(bytes);
#ifdef _WIN32
  file_ = _wfopen(path.c_str(), L"rb");
#else
  file_ = std::fopen(path.c_str(), "rb");
#endif
  if (!file_)
    throw PlayerError("playback/file.source: cannot open file");
  m_eventBus->subscribe<FileIORequest>(getName() + ".io", [this](FileIORequest& request) {
    request.handled = true;
    try {
      handle_io(request);
    } catch (...) {
      request.error = std::current_exception();
    }
  });
  finished_ = true;  // Demand-driven byte service; stream EOS belongs to demux.
  return true;
}

void FileSourceNode::handle_io(FileIORequest& request) {
  if (request.kind == FileIORequest::begin) {
    begin_io();
    check_cancelled();
    return;
  }
  check_cancelled();
  if (request.kind == FileIORequest::read) {
    if (request.size <= 0)
      throw PlayerError("playback/file.source: invalid read size");
    auto bytes = std::make_shared<std::vector<uint8_t>>(std::min(request.size, 32768));
    const auto count = std::fread(bytes->data(), 1, bytes->size(), file_);
    check_cancelled();
    if (!count) {
      request.result = std::ferror(file_) ? AVERROR(EIO) : AVERROR_EOF;
      return;
    }
    bytes->resize(count);
    SeekableBytes event;
    event.bytes = std::move(bytes);
    if (!publish_media(std::move(event)))
      throw PlayerError("playback/file.source: bytes were not consumed");
    request.result = static_cast<int64_t>(count);
    return;
  }
  if (request.whence & AVSEEK_SIZE) {
    request.result = size_;
    return;
  }
  const int whence = request.whence & ~AVSEEK_FORCE;
  if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
    request.result = AVERROR(EINVAL);
    return;
  }
#ifdef _WIN32
  request.result = _fseeki64(file_, request.offset, whence) == 0 ? _ftelli64(file_) : AVERROR(EIO);
#else
  request.result = fseeko(file_, request.offset, whence) == 0 ? ftello(file_) : AVERROR(EIO);
#endif
}

void FileSourceNode::doRelease() {
  m_eventBus->unsubscribe(getName() + ".io");
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
}
}  // namespace livekit::sdk::detail
