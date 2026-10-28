#include <livekit/livekit.hpp>
#include <fstream>
#include <iostream>
#include <thread>
using namespace livekit::sdk;
using namespace std::chrono_literals;
int main(int argc,char** argv){try{
  if(argc<4)throw std::invalid_argument("url output-directory mode(plain|encrypted|single)");
  const std::filesystem::path out=argv[2];const std::string mode=argv[3];
  auto unhex=[](const std::string& s){std::vector<uint8_t> result;for(size_t i=0;i<s.size();i+=2)result.push_back(uint8_t(std::stoul(s.substr(i,2),nullptr,16)));return result;};
  auto hex=[](const std::vector<uint8_t>& data){std::string result;for(auto v:data){result+="0123456789abcdef"[v>>4];result+="0123456789abcdef"[v&15];}return result;};
  auto vector_keys=std::make_shared<ExternalKeyProvider>();EncryptionKey zero{};vector_keys->set_key(0,zero);
  Json vectors;{std::ifstream f(out/"go-vectors.json");f>>vectors;}
  for(auto& vector:vectors){const auto name=vector.at("codec").get<std::string>();FrameCryptor cryptor(vector_keys,name=="opus"?EncryptionCodec::opus:name=="h264"?EncryptionCodec::h264:EncryptionCodec::h265);auto plain=unhex(vector.at("plain"));if(cryptor.decrypt(unhex(vector.at("sealed")))!=plain)throw std::runtime_error("Go media crypto incompatible: "+name);vector["sealed"]=hex(cryptor.encrypt(plain));}
  {std::ofstream f(out/"cpp-vectors.json");f<<vectors;}
  const bool mediaMode=mode=="encoded"||mode=="media-e2ee";
  Room room;std::promise<std::string> text;std::promise<std::vector<uint8_t>> binary;
  room.register_text_stream_handler("reply-text",[&](auto reader){text.set_value(reader->read_all_text());});
  room.register_byte_stream_handler("reply-bytes",[&](auto reader){binary.set_value(reader->read_all());});
  RoomOptions options;options.signal_protocol=17;options.single_peer_connection=mode=="single";options.control_callback_thread=true;
  options.packet_stream=std::make_shared<PacketStream>();
  options.log_sink=[](const Json& event){auto diagnostic=event;diagnostic.erase("payload");diagnostic.erase("payloadBase64");std::cerr<<diagnostic.dump()<<'\n';};
  if(mediaMode){options.encoded_frame_stream=std::make_shared<EncodedMediaStream>();options.video_frame_stream=std::make_shared<VideoFrameStream>();}
  if(mode=="encrypted"||mode=="media-e2ee"){auto keys=std::make_shared<ExternalKeyProvider>();EncryptionKey key{};keys->set_key(0,key);options.encryption.keys=keys;options.encryption.media=mode=="media-e2ee";}
  auto token=AccessToken("reconnect-test","reconnect-integration-secret-32-characters").set_identity("cpp-peer").set_video_grant({{"roomJoin",true},{"room","extensions"}}).to_jwt();
  if(mode=="single"){MediaSpec initial;initial.demo=true;initial.width=160;initial.height=120;options.media.push_back(initial);}
  room.join_with_token(argv[1],token,options);
  if(mode=="backup"){MediaSpec media;media.demo=true;media.width=160;media.height=120;media.dynacast=true;TrackPublishOptions publication;publication.name="cpp-backup";publication.codec="h265";publication.backup_codec=BackupCodecOptions{};room.local_participant().publish_track(media,publication).get();}
  std::exception_ptr pumpError;std::jthread encodedPump;
  if(mediaMode){auto file=std::make_shared<EncodedFileReader>(out/"source.h264");auto source=std::make_shared<EncodedMediaSource>(file->source_options());MediaSpec media;media.width=source->options().width;media.height=source->options().height;media.encoded_source=source;TrackPublishOptions pub;pub.name="cpp-encoded";room.local_participant().publish_track(media,pub).get();encodedPump=std::jthread([file,source,&pumpError](std::stop_token stop){try{file->pump(source,stop);}catch(const std::exception& error){if(!stop.stop_requested()){pumpError=std::current_exception();std::cerr<<"Encoded file pump: "<<error.what()<<std::endl;}}});}
  StreamOptions stream;stream.topic="request-text";stream.destination_identities={"go-peer"};
  std::string expected;for(int i=0;i<10000;++i)expected+="中文🌍";
  room.local_participant().send_text(expected,stream).get();
  std::vector<uint8_t> payload(300000);for(size_t i=0;i<payload.size();++i)payload[i]=uint8_t(i*31);
  auto file=out/"input.bin";{std::ofstream f(file,std::ios::binary);f.write(reinterpret_cast<const char*>(payload.data()),payload.size());}
  stream.topic="request-bytes";room.local_participant().send_file(file,stream).get();
  auto t=text.get_future();auto b=binary.get_future();
  if(t.wait_for(20s)!=std::future_status::ready||b.wait_for(20s)!=std::future_status::ready||t.get()!=expected||b.get()!=payload)throw std::runtime_error("Go stream echo content mismatch/timeout");
  auto tracks=room.remote_data_tracks();if(tracks.empty())throw std::runtime_error("Go DataTrack missing from initial roster");
  auto dataReader=tracks.front().subscribe().get();auto localData=room.local_participant().publish_data_track("cpp-data").get();
  DataFrame data;data.payload.resize(40000);data.user_timestamp=12345;for(size_t i=0;i<data.payload.size();++i)data.payload[i]=uint8_t(i*17);
  bool echoed=false;for(int i=0;i<100&&!echoed;++i){localData.write(data);try{auto frame=dataReader->read(100ms);if(frame){if(frame->payload!=data.payload||frame->user_timestamp!=data.user_timestamp)throw std::runtime_error("Go DataTrack echo mismatch");echoed=true;}}catch(const SDKError& e){if(e.code()!=ErrorCode::timeout)throw;}}
  if(!echoed)throw std::runtime_error("Go DataTrack echo timed out");
  if(mode=="single"||mode=="backup"||mediaMode){const auto deadline=std::chrono::steady_clock::now()+20s;while(!std::filesystem::exists(out/"go-video")){if(std::chrono::steady_clock::now()>=deadline)throw std::runtime_error("Go video reception timed out");std::this_thread::sleep_for(50ms);}}
  if(mediaMode){auto encoded=options.encoded_frame_stream->read(10s);auto pixels=options.video_frame_stream->read(10s);if(!encoded||encoded->data.empty()||pixels.status!=VideoReadStatus::frame||pixels.frame->width!=160||pixels.frame->height!=120)throw std::runtime_error("Encoded echo did not decode");encodedPump.request_stop();encodedPump.join();if(pumpError)std::rethrow_exception(pumpError);}
  if(mode=="single"||mode=="backup"||mediaMode){auto packet=options.packet_stream->read(5s);if(!packet||packet->bytes.empty())throw std::runtime_error("Native RTP/RTCP observer empty");}
  std::ofstream(out/"cpp-result.json")<<Json{{"ok",true},{"mode",mode},{"textBytes",expected.size()},{"fileBytes",payload.size()},{"dataTrackBytes",data.payload.size()}}.dump(2);
  std::ofstream(out/"cpp-done")<<"done";room.disconnect();return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
