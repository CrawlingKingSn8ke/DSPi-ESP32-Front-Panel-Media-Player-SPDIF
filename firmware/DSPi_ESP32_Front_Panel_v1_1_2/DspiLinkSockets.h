#pragma once
#include <stdint.h>
#include <stddef.h>
namespace DspiLink {
struct SocketToken {int slot=-1;uint32_t generation=0;bool valid()const{return slot>=0&&slot<2&&generation!=0;}};
struct SocketControl {uint16_t code=0;bool release=false;};
// Network adapter serialises access with its mutex. Closed slots stay reserved
// until the hub has received CLOSED and acknowledged release of this generation.
class SocketSlots {
 public:
  SocketToken open(uint32_t now){
    for(int i=0;i<2;++i)if(states_[i].phase==Free){
      auto& s=states_[i];if(++s.generation==0)++s.generation;s.phase=Open;
      s.bytes=0;s.frames=0;s.inboundBytes=0;s.inboundMessages=0;s.progress=now;s.control={};return {i,s.generation};
    }
    return {};
  }
  bool isOpen(SocketToken token)const{return matches(token)&&states_[token.slot].phase==Open;}
  bool inbound(SocketToken token,size_t bytes){
    if(!isOpen(token)||bytes>16384)return false;auto& s=states_[token.slot];
    if(s.inboundMessages>=8||s.inboundBytes+bytes>16384)return false;
    ++s.inboundMessages;s.inboundBytes+=bytes;return true;
  }
  void handled(SocketToken token,size_t bytes){
    if(!matches(token))return;auto& s=states_[token.slot];
    if(s.inboundMessages)--s.inboundMessages;s.inboundBytes=bytes>s.inboundBytes?0:s.inboundBytes-bytes;
  }
  bool reserve(SocketToken token,size_t bytes,uint32_t now){
    if(!isOpen(token)||bytes>16384)return false;
    auto& s=states_[token.slot];
    if(s.frames>=32||s.bytes+bytes>32768)return false;
    if(!s.frames)s.progress=now;
    ++s.frames;s.bytes+=bytes;return true;
  }
  void consumed(SocketToken token,size_t bytes,uint32_t now){
    if(!matches(token))return;auto& s=states_[token.slot];
    if(s.frames)--s.frames;s.bytes=bytes>s.bytes?0:s.bytes-bytes;s.progress=now;
  }
  void close(SocketToken token){if(matches(token))states_[token.slot].phase=Closing;}
  void progress(SocketToken token,uint32_t now){if(matches(token))states_[token.slot].progress=now;}
  bool requestClose(SocketToken token,uint16_t code){
    if(!isOpen(token))return false;auto& s=states_[token.slot];
    if(!s.control.code)s.control.code=code;s.phase=Closing;return true;
  }
  bool requestRelease(SocketToken token){if(!matches(token))return false;states_[token.slot].control.release=true;return true;}
  bool takeControl(SocketToken token,SocketControl& out){
    if(!matches(token))return false;auto& s=states_[token.slot];out=s.control;
    s.control={};return out.code||out.release;
  }
  void release(SocketToken token){if(matches(token)&&states_[token.slot].phase==Closing)states_[token.slot].phase=Free;}
  bool stalled(SocketToken token,uint32_t now)const {
    return isOpen(token)&&states_[token.slot].frames&&uint32_t(now-states_[token.slot].progress)>=10000;
  }
 private:
  enum Phase:uint8_t {Free,Open,Closing};
  struct Slot{uint32_t generation=0,progress=0;size_t bytes=0,inboundBytes=0;unsigned frames=0,inboundMessages=0;Phase phase=Free;SocketControl control{};};
  Slot states_[2];
  bool matches(SocketToken token)const{return token.valid()&&states_[token.slot].phase!=Free&&states_[token.slot].generation==token.generation;}
};
}
