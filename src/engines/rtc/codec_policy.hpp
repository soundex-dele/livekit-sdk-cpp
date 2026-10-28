#pragma once
#include "livekit.pb.h"
#include "api/rtp_parameters.h"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace livekit::sdk::detail {
inline bool disabled_codec(const livekit::ClientConfiguration& config,const webrtc::RtpCodecCapability& codec,bool publish){
  auto lower=[](std::string s){std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return char(std::tolower(c));});return s;};
  auto matches=[&](const livekit::Codec& disabled){
    auto mime=lower(disabled.mime());const auto slash=mime.find('/');if(slash!=std::string::npos)mime.erase(0,slash+1);
    if(mime!=lower(codec.name))return false;
    std::istringstream tokens(disabled.fmtp_line());std::string token;
    while(std::getline(tokens,token,';')){
      const auto begin=token.find_first_not_of(" \t");if(begin==std::string::npos)continue;token.erase(0,begin);token.erase(token.find_last_not_of(" \t")+1);
      const auto eq=token.find('=');if(eq==std::string::npos)return false;
      auto it=codec.parameters.find(token.substr(0,eq));if(it==codec.parameters.end()||it->second!=token.substr(eq+1))return false;
    }return true;
  };
  for(const auto& disabled:config.disabled_codecs().codecs())if(matches(disabled))return true;
  if(publish)for(const auto& disabled:config.disabled_codecs().publish())if(matches(disabled))return true;
  return false;
}
}
