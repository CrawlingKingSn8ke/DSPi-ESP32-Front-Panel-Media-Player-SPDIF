#pragma once
#include <stdint.h>
#include <stddef.h>
namespace DspiLink {
class SpectrumDemand {
 public:
  void note(unsigned slot,uint32_t now){if(slot<2){seen_[slot]=true;last_[slot]=now;}}
  void closed(unsigned slot){if(slot<2)seen_[slot]=false;}
  bool active(uint32_t now)const{for(unsigned i=0;i<2;++i)if(seen_[i]&&uint32_t(now-last_[i])<5000)return true;return false;}
  static bool compatible(const uint8_t* wire,size_t size,uint8_t tap,uint16_t channels){
    return wire&&size==12&&wire[0]==3&&wire[1]==tap&&((wire[2]|(uint16_t(wire[3])<<8))&channels)==channels;
  }
 private:bool seen_[2]{};uint32_t last_[2]{};
};
class NotificationSequence {
 public:
  bool gap(const uint8_t* p,size_t size){
    if(p&&size&&p[0]==1)return false; // Legacy v1 packets have no v2 sequence.
    if(!p||size<4||p[0]!=2)return true;
    bool missed=known_&&uint8_t(last_+1)!=p[3];last_=p[3];known_=true;return missed;
  }
  void reset(){known_=false;}
 private:bool known_=false;uint8_t last_=0;
};
}
