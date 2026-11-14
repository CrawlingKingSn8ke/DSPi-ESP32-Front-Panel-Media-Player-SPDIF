#pragma once
#include <string.h>
#include <stdint.h>
namespace DspiLink {
// Accidental-image protection, not a cryptographic signature. Keep the
// historical sketch/project identity across combined-panel OTA releases.
inline bool panelProjectMatches(const char candidate[32],const char current[32]){
  const size_t length=strnlen(current,32),other=strnlen(candidate,32);
  return length&&length<32&&other==length&&!memcmp(candidate,current,length);
}
class PanelImageIdentity {
 public:
  void reset(){matched_=0;found_=false;}
  void feed(const uint8_t* bytes,size_t size){
    // This existing serial banner is also present in recovery v1.4.0 and
    // older panel images. The SDK's arduino-lib-builder name is not unique.
    static constexpr char marker[]="DSPi ESP32 Control Panel v";
    if(found_)return;
    for(size_t i=0;i<size;++i){
      if(bytes[i]==uint8_t(marker[matched_]))++matched_;
      else matched_=bytes[i]==uint8_t(marker[0])?1:0;
      if(matched_==sizeof(marker)-1){found_=true;return;}
    }
  }
  bool matches()const{return found_;}
 private:size_t matched_=0;bool found_=false;
};
}
