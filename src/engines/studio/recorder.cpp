#include "internal.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <thread>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
namespace livekit::sdk::studio_detail {
static void check(int e){if(e<0){char s[256]{};av_strerror(e,s,sizeof(s));throw std::runtime_error(std::string("录制失败: ")+s);}}
static std::string path8(const std::filesystem::path&p){auto s=p.u8string();return {s.begin(),s.end()};}
struct Encoder{
 std::filesystem::path path,mkv;AVFormatContext*format=nullptr;AVCodecContext*video=nullptr,*audio=nullptr;AVStream*vs=nullptr,*as=nullptr;AVFrame*vf=nullptr,*af=nullptr;SwsContext*scale=nullptr;AVAudioFifo*fifo=nullptr;int64_t video_pts=0,audio_pts=0;bool done=false;std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
 Encoder(const std::filesystem::path&p,int w,int h,int fps):path(p),mkv(p){
  mkv.replace_extension(".mkv");if(std::filesystem::exists(path)||std::filesystem::exists(mkv))throw std::runtime_error("录制文件已存在");if(!p.parent_path().empty())std::filesystem::create_directories(p.parent_path());
  try{check(avformat_alloc_output_context2(&format,nullptr,"matroska",path8(mkv).c_str()));auto*vc=avcodec_find_encoder_by_name("libx264");if(!vc)vc=avcodec_find_encoder(AV_CODEC_ID_H264);auto*ac=avcodec_find_encoder(AV_CODEC_ID_AAC);if(!vc||!ac)throw std::runtime_error("FFmpeg 缺少 H.264/AAC 编码器");video=avcodec_alloc_context3(vc);audio=avcodec_alloc_context3(ac);if(!video||!audio)throw std::bad_alloc();video->width=w;video->height=h;video->pix_fmt=AV_PIX_FMT_YUV420P;video->time_base={1,fps};video->framerate={fps,1};video->bit_rate=4000000;video->gop_size=fps*2;video->max_b_frames=0;video->thread_count=2;if(video->priv_data){av_opt_set(video->priv_data,"preset","veryfast",0);av_opt_set(video->priv_data,"tune","zerolatency",0);}audio->sample_rate=48000;audio->sample_fmt=AV_SAMPLE_FMT_FLTP;audio->ch_layout=AV_CHANNEL_LAYOUT_STEREO;audio->time_base={1,48000};audio->bit_rate=128000;if(format->oformat->flags&AVFMT_GLOBALHEADER){video->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;audio->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;}check(avcodec_open2(video,vc,nullptr));check(avcodec_open2(audio,ac,nullptr));vs=avformat_new_stream(format,nullptr);as=avformat_new_stream(format,nullptr);if(!vs||!as)throw std::bad_alloc();vs->time_base=video->time_base;as->time_base=audio->time_base;check(avcodec_parameters_from_context(vs->codecpar,video));check(avcodec_parameters_from_context(as->codecpar,audio));check(avio_open(&format->pb,path8(mkv).c_str(),AVIO_FLAG_WRITE));check(avformat_write_header(format,nullptr));vf=av_frame_alloc();af=av_frame_alloc();if(!vf||!af)throw std::bad_alloc();vf->format=video->pix_fmt;vf->width=w;vf->height=h;check(av_frame_get_buffer(vf,32));af->format=audio->sample_fmt;af->sample_rate=48000;check(av_channel_layout_copy(&af->ch_layout,&audio->ch_layout));af->nb_samples=audio->frame_size;check(av_frame_get_buffer(af,0));fifo=av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP,2,4096);if(!fifo)throw std::bad_alloc();scale=sws_getContext(w,h,AV_PIX_FMT_RGBA,w,h,AV_PIX_FMT_YUV420P,SWS_BILINEAR,nullptr,nullptr,nullptr);if(!scale)throw std::runtime_error("录制颜色转换失败");start=std::chrono::steady_clock::now();}catch(...){release();throw;}
 }
 void release(){if(format){if(format->pb)avio_closep(&format->pb);avformat_free_context(format);format=nullptr;}avcodec_free_context(&video);avcodec_free_context(&audio);av_frame_free(&vf);av_frame_free(&af);sws_freeContext(scale);scale=nullptr;if(fifo)av_audio_fifo_free(fifo);fifo=nullptr;}
 ~Encoder(){release();}
 void encode(AVCodecContext*c,AVStream*s,AVFrame*f){check(avcodec_send_frame(c,f));AVPacket*p=av_packet_alloc();if(!p)throw std::bad_alloc();try{for(;;){int e=avcodec_receive_packet(c,p);if(e==AVERROR(EAGAIN)||e==AVERROR_EOF)break;check(e);av_packet_rescale_ts(p,c->time_base,s->time_base);p->stream_index=s->index;check(av_interleaved_write_frame(format,p));av_packet_unref(p);}}catch(...){av_packet_free(&p);throw;}av_packet_free(&p);}
 void writeVideo(const StudioImage&i,std::chrono::steady_clock::time_point captured){check(av_frame_make_writable(vf));const uint8_t*data=i.rgba.data();int stride=i.width*4;sws_scale(scale,&data,&stride,0,i.height,vf->data,vf->linesize);auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(captured-start).count();int64_t tick=av_rescale_q(elapsed,{1,1000000},video->time_base);vf->pts=std::max(video_pts,tick);video_pts=vf->pts+1;encode(video,vs,vf);}
 void writeAudio(std::span<const float>samples,std::chrono::steady_clock::time_point captured){
  auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(captured-start).count();int64_t target=elapsed*48000/1000000;
  int64_t queued=audio_pts+av_audio_fifo_size(fifo);size_t count=samples.size()/2;
  if(target+int64_t(count)<=queued)return;
  if(target<queued){auto skip=size_t(queued-target);samples=samples.subspan(skip*2);count-=skip;target=queued;}
  if(target-queued>240000)throw std::runtime_error("Recording audio clock jumped by over five seconds; MKV retained");
  // Align PCM to the same captured timestamp as video, including exact silence
  // gaps. Encoder scheduling latency must never shift the audio timeline.
  if(target>queued){std::vector<float>silence(4800);void*planes[]{silence.data(),silence.data()};while(target>queued){int n=int(std::min<int64_t>(target-queued,4800));check(av_audio_fifo_write(fifo,planes,n));drain(false);queued+=n;}}
  std::vector<float>l(count),r(count);for(size_t i=0;i<count;++i){l[i]=samples[i*2];r[i]=samples[i*2+1];}void*planes[]{l.data(),r.data()};check(av_audio_fifo_write(fifo,planes,int(count)));drain(false);}
 void drain(bool final){while(av_audio_fifo_size(fifo)>=af->nb_samples||(final&&av_audio_fifo_size(fifo)>0)){check(av_frame_make_writable(af));int n=std::min(av_audio_fifo_size(fifo),af->nb_samples);for(int c=0;c<2;++c)memset(af->data[c],0,size_t(af->nb_samples)*sizeof(float));check(av_audio_fifo_read(fifo,reinterpret_cast<void**>(af->data),n));af->pts=audio_pts;audio_pts+=af->nb_samples;encode(audio,as,af);}}
 void finish(){if(done)return;drain(true);encode(video,vs,nullptr);encode(audio,as,nullptr);check(av_write_trailer(format));check(avio_closep(&format->pb));done=true;
  AVFormatContext*in=nullptr,*out=nullptr;AVPacket*p=nullptr;auto temp=path;temp+=".partial";
  try{check(avformat_open_input(&in,path8(mkv).c_str(),nullptr,nullptr));check(avformat_find_stream_info(in,nullptr));check(avformat_alloc_output_context2(&out,nullptr,"mp4",path8(temp).c_str()));for(unsigned i=0;i<in->nb_streams;++i){auto*s=avformat_new_stream(out,nullptr);if(!s)throw std::bad_alloc();check(avcodec_parameters_copy(s->codecpar,in->streams[i]->codecpar));s->codecpar->codec_tag=0;s->time_base=in->streams[i]->time_base;}check(avio_open(&out->pb,path8(temp).c_str(),AVIO_FLAG_WRITE));check(avformat_write_header(out,nullptr));p=av_packet_alloc();if(!p)throw std::bad_alloc();int e;while((e=av_read_frame(in,p))>=0){auto index=p->stream_index;av_packet_rescale_ts(p,in->streams[index]->time_base,out->streams[index]->time_base);p->pos=-1;check(av_interleaved_write_frame(out,p));av_packet_unref(p);}if(e!=AVERROR_EOF)check(e);check(av_write_trailer(out));check(avio_closep(&out->pb));av_packet_free(&p);avformat_close_input(&in);avformat_free_context(out);out=nullptr;std::filesystem::rename(temp,path);
  }catch(...){av_packet_free(&p);avformat_close_input(&in);if(out){if(out->pb)avio_closep(&out->pb);avformat_free_context(out);}throw;}
 }
};
struct Recorder::Impl {
 struct Packet {std::shared_ptr<StudioImage>image;std::vector<float>audio;std::chrono::steady_clock::time_point time;};
 Encoder encoder;std::mutex mutex;std::condition_variable wake;std::deque<Packet>queue;std::jthread worker;bool closing=false;size_t videos=0;std::exception_ptr error;
 Impl(const std::filesystem::path&p,int w,int h,int fps):encoder(p,w,h,fps){worker=std::jthread([this]{try{for(;;){Packet packet;{std::unique_lock lock(mutex);wake.wait(lock,[&]{return closing||!queue.empty();});if(queue.empty())break;packet=std::move(queue.front());queue.pop_front();if(packet.image)--videos;}if(packet.image)encoder.writeVideo(*packet.image,packet.time);else encoder.writeAudio(packet.audio,packet.time);}encoder.finish();}catch(...){std::lock_guard lock(mutex);error=std::current_exception();queue.clear();closing=true;}});}
 ~Impl(){try{finish();}catch(...){}}
 void push(Packet packet){std::lock_guard lock(mutex);if(error)std::rethrow_exception(error);if(closing)throw std::runtime_error("Recorder is closed");if(packet.image&&videos>=6)return;if(queue.size()>=256)throw std::runtime_error("Recording storage cannot keep up; MKV retained");if(packet.image)++videos;queue.push_back(std::move(packet));wake.notify_one();}
 void finish(){{std::lock_guard lock(mutex);closing=true;}wake.notify_one();if(worker.joinable())worker.join();if(error)std::rethrow_exception(error);}
};
Recorder::Recorder(const std::filesystem::path&p,int w,int h,int fps):impl_(std::make_unique<Impl>(p,w,h,fps)){}
Recorder::~Recorder()=default;
void Recorder::video(const StudioImage&i,std::chrono::steady_clock::time_point captured){impl_->push({std::make_shared<StudioImage>(i),{},captured});}
void Recorder::audio(std::span<const float>s,std::chrono::steady_clock::time_point captured){impl_->push({{},std::vector<float>(s.begin(),s.end()),captured});}
void Recorder::finish(){impl_->finish();}
}
