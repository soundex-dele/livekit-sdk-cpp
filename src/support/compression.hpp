#pragma once
#include <zlib.h>
#include <array>
#include <span>
#include <stdexcept>
#include <string>

namespace livekit::sdk::detail {
class InflateRaw {
 public:
  InflateRaw(){if(inflateInit2(&state_,-MAX_WBITS)!=Z_OK)throw std::runtime_error("Cannot initialize DEFLATE");}
  ~InflateRaw(){inflateEnd(&state_);}
  InflateRaw(const InflateRaw&)=delete;
  std::string append(std::span<const uint8_t> input,size_t limit,bool final=false){
    if(input.size()>1024*1024|| (ended_&&!input.empty()))throw std::invalid_argument("Invalid compressed payload");
    if(ended_)return {};
    state_.next_in=const_cast<Bytef*>(input.data());state_.avail_in=static_cast<uInt>(input.size());std::string output;std::array<char,8192> buffer;
    for(;;){state_.next_out=reinterpret_cast<Bytef*>(buffer.data());state_.avail_out=static_cast<uInt>(buffer.size());const auto result=inflate(&state_,final?Z_FINISH:Z_NO_FLUSH);
      const auto bytes=buffer.size()-state_.avail_out;if(bytes>limit-output.size())throw std::length_error("Decompressed payload exceeds limit");output.append(buffer.data(),bytes);
      if(result==Z_STREAM_END){ended_=true;if(state_.avail_in)throw std::invalid_argument("Trailing compressed data");break;}
      if(result!=Z_OK&&result!=Z_BUF_ERROR)throw std::invalid_argument("Malformed DEFLATE data");
      if(state_.avail_in==0&&state_.avail_out){if(final)throw std::invalid_argument("Truncated DEFLATE data");break;}
      if(result==Z_BUF_ERROR&&bytes==0)throw std::invalid_argument("Stalled DEFLATE stream");
    }return output;
  }
 private:z_stream state_{};bool ended_=false;
};
inline std::string inflate_raw(const std::string& input,size_t limit){InflateRaw inflater;return inflater.append({reinterpret_cast<const uint8_t*>(input.data()),input.size()},limit,true);}
inline std::string deflate_raw(const std::string& input){
  if(input.size()>1024*1024)throw std::length_error("Compression input limit");z_stream state{};
  if(deflateInit2(&state,Z_DEFAULT_COMPRESSION,Z_DEFLATED,-MAX_WBITS,8,Z_DEFAULT_STRATEGY)!=Z_OK)throw std::runtime_error("Cannot initialize DEFLATE encoder");
  struct End{z_stream* state;~End(){deflateEnd(state);}}end{&state};
  std::string output(deflateBound(&state,static_cast<uLong>(input.size())),0);state.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));state.avail_in=static_cast<uInt>(input.size());state.next_out=reinterpret_cast<Bytef*>(output.data());state.avail_out=static_cast<uInt>(output.size());
  if(deflate(&state,Z_FINISH)!=Z_STREAM_END)throw std::runtime_error("DEFLATE failed");output.resize(state.total_out);return output;
}
}
