#pragma once
#include "livekit/operation.hpp"
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace livekit::sdk {
namespace detail {class DataTrackSession;struct DataFrameQueue;}
struct DataEncoding {int well_known=0;std::string custom;};
struct DataSchema {std::string name;DataEncoding encoding;};
struct DataTrackOptions {
  std::optional<DataEncoding> frame_encoding;
  std::optional<DataSchema> schema;
};
struct DataTrackInfo {
  std::string sid,name,publisher_identity;
  DataTrackOptions options;
  bool encrypted=false;
};
struct DataFrame {std::vector<uint8_t> payload;std::optional<uint64_t> user_timestamp;};
struct DataTrackStats {uint64_t accepted=0,delivered=0,dropped=0,expired=0,malformed=0;};
struct DataSubscribeOptions {size_t buffer_frames=8;std::optional<uint32_t> target_fps;};
class DataTrackStream {
 public:
  ~DataTrackStream();
  std::optional<DataFrame> read(std::chrono::milliseconds timeout=std::chrono::seconds(30),std::stop_token stop={});
  void close();
  DataTrackStats stats() const;
 private:
  explicit DataTrackStream(std::shared_ptr<detail::DataFrameQueue> state):state_(std::move(state)){}
  std::shared_ptr<detail::DataFrameQueue> state_;
  friend class detail::DataTrackSession;
};
class LocalDataTrack {
 public:
  DataTrackInfo info()const;
  // Freshness-oriented, bounded submission. May replace an older unsent frame.
  void write(DataFrame frame)const;
  std::future<void> unpublish(OperationOptions operation={})const;
  DataTrackStats stats()const;
 private:
  LocalDataTrack(std::weak_ptr<detail::DataTrackSession> s,uint64_t generation,uint16_t handle):session_(std::move(s)),generation_(generation),handle_(handle){}
  std::weak_ptr<detail::DataTrackSession> session_;uint64_t generation_;uint16_t handle_;
  friend class detail::DataTrackSession;
};
class RemoteDataTrack {
 public:
  DataTrackInfo info()const{return info_;}
  std::future<std::shared_ptr<DataTrackStream>> subscribe(DataSubscribeOptions options={},OperationOptions operation={})const;
 private:
  RemoteDataTrack(std::weak_ptr<detail::DataTrackSession> s,uint64_t generation,DataTrackInfo info):session_(std::move(s)),generation_(generation),info_(std::move(info)){}
  std::weak_ptr<detail::DataTrackSession> session_;uint64_t generation_;DataTrackInfo info_;
  friend class detail::DataTrackSession;
};
}
