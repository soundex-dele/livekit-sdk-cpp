#include "livekit/encoded_file.hpp"
#include "livekit/error.hpp"
#include <condition_variable>
#include <mutex>
#ifdef LIVEKIT_RTC_FFMPEG_CODECS
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>
}
#endif

namespace livekit::sdk {
struct EncodedFileReader::Impl {
  EncodedSourceOptions options;std::filesystem::path path;
#ifdef LIVEKIT_RTC_FFMPEG_CODECS
  AVFormatContext* format=nullptr;AVBSFContext* filter=nullptr;int stream=-1;bool eof=false;
  std::optional<int64_t> last_us;
  int64_t first_pts=AV_NOPTS_VALUE,next_us=0;std::chrono::steady_clock::time_point epoch=std::chrono::steady_clock::now();
  ~Impl(){av_bsf_free(&filter);avformat_close_input(&format);}
#endif
};
EncodedFileReader::EncodedFileReader(const std::filesystem::path& path,bool video):impl_(std::make_unique<Impl>()){
#ifdef LIVEKIT_RTC_FFMPEG_CODECS
  auto& s=*impl_;s.path=path;const auto u8=path.u8string();const std::string name(u8.begin(),u8.end());
  if(avformat_open_input(&s.format,name.c_str(),nullptr,nullptr)<0||avformat_find_stream_info(s.format,nullptr)<0)throw std::runtime_error("Cannot open encoded media file");
  s.stream=av_find_best_stream(s.format,video?AVMEDIA_TYPE_VIDEO:AVMEDIA_TYPE_AUDIO,-1,-1,nullptr,0);
  if(s.stream<0)throw std::invalid_argument("Requested encoded media stream not found");
  const auto* codec=s.format->streams[s.stream]->codecpar;
  const char* filter=nullptr;
  switch(codec->codec_id){
    case AV_CODEC_ID_H264:s.options.codec="h264";filter="h264_mp4toannexb";break;
    case AV_CODEC_ID_HEVC:s.options.codec="h265";filter="hevc_mp4toannexb";break;
    case AV_CODEC_ID_VP8:s.options.codec="vp8";break;
    case AV_CODEC_ID_OPUS:s.options.codec="opus";break;
    default:throw std::invalid_argument("File codec must be VP8, H264, H265 or Opus");
  }
  if(video){s.options.width=codec->width;s.options.height=codec->height;}
  if(filter){
    const auto* f=av_bsf_get_by_name(filter);if(!f||av_bsf_alloc(f,&s.filter)<0)throw std::runtime_error("Annex-B filter unavailable");
    if(avcodec_parameters_copy(s.filter->par_in,codec)<0)throw std::runtime_error("Cannot configure Annex-B filter");
    s.filter->time_base_in=s.format->streams[s.stream]->time_base;if(av_bsf_init(s.filter)<0)throw std::runtime_error("Cannot initialize Annex-B filter");
  }
#else
  throw SDKError(ErrorDomain::media,ErrorCode::unsupported,"Encoded files require LIVEKIT_RTC_FFMPEG_CODECS");
#endif
}
EncodedFileReader::~EncodedFileReader()=default;
EncodedSourceOptions EncodedFileReader::source_options()const{return impl_->options;}
std::optional<EncodedMediaFrame> EncodedFileReader::read(){
#ifdef LIVEKIT_RTC_FFMPEG_CODECS
  auto& s=*impl_;auto* raw=av_packet_alloc();if(!raw)throw std::bad_alloc();
  const std::unique_ptr<AVPacket,void(*)(AVPacket*)> packet(raw,[](AVPacket* p){av_packet_free(&p);});
  for(;;){
    if(s.filter){const int result=av_bsf_receive_packet(s.filter,raw);if(result==0)break;if(result==AVERROR_EOF)return {};if(result!=AVERROR(EAGAIN))throw std::runtime_error("Annex-B conversion failed");}
    if(s.eof)return {};
    int result=av_read_frame(s.format,raw);
    if(result==AVERROR_EOF){s.eof=true;if(s.filter){av_bsf_send_packet(s.filter,nullptr);continue;}return {};}
    if(result<0)throw std::runtime_error("Encoded file read failed");
    if(raw->stream_index!=s.stream){av_packet_unref(raw);continue;}
    if(!s.filter)break;
    if(av_bsf_send_packet(s.filter,raw)<0)throw std::runtime_error("Annex-B input failed");
  }
  const auto timebase=s.filter?s.filter->time_base_out:s.format->streams[s.stream]->time_base;
  auto pts=raw->pts==AV_NOPTS_VALUE?raw->dts:raw->pts;
  if(s.first_pts==AV_NOPTS_VALUE&&pts!=AV_NOPTS_VALUE)s.first_pts=pts;
  const auto us=pts==AV_NOPTS_VALUE?s.next_us:av_rescale_q(pts-s.first_pts,timebase,AVRational{1,1000000});
  auto duration=raw->duration>0?av_rescale_q(raw->duration,timebase,AVRational{1,1000000}):s.options.codec=="opus"?20000:33333;
  if(us<0||(s.last_us&&us<=*s.last_us)||duration<1||duration>1000000)throw std::invalid_argument("Non-monotonic encoded access units; B-frames are not supported");
  s.last_us=us;s.next_us=us+duration;EncodedMediaFrame frame;frame.codec=s.options.codec;frame.width=s.options.width;frame.height=s.options.height;
  frame.data.assign(raw->data,raw->data+raw->size);frame.keyframe=raw->flags&AV_PKT_FLAG_KEY;frame.duration=std::chrono::microseconds(duration);frame.captured=s.epoch+std::chrono::microseconds(us);return frame;
#else
  return {};
#endif
}
void EncodedFileReader::rewind(){
#ifdef LIVEKIT_RTC_FFMPEG_CODECS
  auto& s=*impl_;if(av_seek_frame(s.format,s.stream,0,AVSEEK_FLAG_BACKWARD)<0){EncodedFileReader reopened(s.path,s.options.codec!="opus");impl_=std::move(reopened.impl_);return;}
  if(s.filter)av_bsf_flush(s.filter);s.first_pts=AV_NOPTS_VALUE;s.last_us.reset();s.next_us=0;s.eof=false;s.epoch=std::chrono::steady_clock::now();
#endif
}
void EncodedFileReader::pump(const std::shared_ptr<EncodedMediaSource>& source,std::stop_token stop){
  if(!source||source->options().codec!=impl_->options.codec)throw std::invalid_argument("Encoded file source mismatch");
  std::mutex mutex;std::condition_variable_any ready;std::unique_lock lock(mutex);
  auto wait=[&](std::chrono::steady_clock::time_point time){ready.wait_until(lock,stop,time,[]{return false;});if(stop.stop_requested())throw SDKError(ErrorDomain::media,ErrorCode::cancelled,"Encoded file pump cancelled");};
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
  while(!source->stats().accepting){if(source->stats().closed||std::chrono::steady_clock::now()>deadline)throw std::runtime_error("Encoded source is not active");wait(std::chrono::steady_clock::now()+std::chrono::milliseconds(5));}
  const auto generation=source->stats().generation;rewind();
  while(auto frame=read()){
    wait(frame->captured);
    for(;;){auto stats=source->stats();if(!stats.accepting||stats.generation!=generation)throw SDKError(ErrorDomain::media,ErrorCode::session_closed,"Encoded source generation changed");
      if(stats.keyframe_required&&!frame->keyframe&&frame->codec!="opus")break;
      const auto status=source->write(*frame);if(status==EncodedWriteStatus::accepted||status==EncodedWriteStatus::keyframe_required)break;
      if(status==EncodedWriteStatus::closed)throw std::runtime_error("Encoded source closed");
      if(std::chrono::steady_clock::now()-frame->captured>source->options().max_age){source->request_keyframe();break;}
      wait(std::chrono::steady_clock::now()+std::chrono::milliseconds(2));
    }
  }
}
}
