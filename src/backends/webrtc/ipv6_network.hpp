#pragma once
#include "rtc_base/network.h"
#include "api/environment/environment_factory.h"
#include <algorithm>

namespace livekit::sdk::detail {
class IPv6NetworkManager final:public webrtc::BasicNetworkManager {
 public:
  explicit IPv6NetworkManager(webrtc::SocketFactory* socket):webrtc::BasicNetworkManager(webrtc::CreateEnvironment(),socket){}
  std::vector<const webrtc::Network*> GetNetworks()const override{return filter(webrtc::BasicNetworkManager::GetNetworks());}
  std::vector<const webrtc::Network*> GetAnyAddressNetworks()override{return filter(webrtc::BasicNetworkManager::GetAnyAddressNetworks());}
  bool GetDefaultLocalAddress(int family,webrtc::IPAddress* ip)const override{return family==AF_INET6&&webrtc::BasicNetworkManager::GetDefaultLocalAddress(family,ip);}
 private:
  static std::vector<const webrtc::Network*> filter(std::vector<const webrtc::Network*> networks){std::erase_if(networks,[](auto* n){return n->family()!=AF_INET6;});return networks;}
};
}
