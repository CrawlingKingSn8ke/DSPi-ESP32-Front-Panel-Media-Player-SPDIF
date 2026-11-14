#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace PanelWifi {
struct Credentials {char ssid[33]{},password[65]{};};
enum class SetupPhase:uint8_t {Closed,Ready,Joining,SaveRequested,Success,Failed};
inline bool validCredentials(const Credentials& c){
  const size_t s=strnlen(c.ssid,sizeof c.ssid),p=strnlen(c.password,sizeof c.password);
  return s>0&&s<sizeof c.ssid&&p<=63&&(p==0||p>=8);
}

// Physical setup is separate from Link pairing. A fresh 128-bit capability
// lives only for this setup session and is carried by the second QR URL.
class SetupPolicy {
 public:
  void open(const uint8_t entropy[16],uint32_t now){
    cancel();static const char digits[]="0123456789abcdef";
    for(unsigned n=0;n<16;++n){token_[n*2]=digits[entropy[n]>>4];token_[n*2+1]=digits[entropy[n]&15];}
    opened_=now;phase_=SetupPhase::Ready;
  }
  bool active(uint32_t now)const{return phase_!=SetupPhase::Closed&&uint32_t(now-opened_)<600000;}
  bool authorized(const char* supplied,uint32_t now)const{
    if(!active(now)||!supplied||strnlen(supplied,33)!=32)return false;
    unsigned difference=0;for(unsigned n=0;n<32;++n)difference|=uint8_t(supplied[n])^uint8_t(token_[n]);
    return difference==0;
  }
  void copyToken(char (&out)[33])const{memcpy(out,token_,sizeof token_);}
  SetupPhase phase()const{return phase_;}
  const Credentials& candidate()const{return candidate_;}
  bool request(const Credentials& c,const char* supplied,uint32_t now){
    if(!authorized(supplied,now)||!validCredentials(c)||
       (phase_!=SetupPhase::Ready&&phase_!=SetupPhase::Failed))return false;
    candidate_=c;joinAt_=now;joinPending_=true;phase_=SetupPhase::Joining;return true;
  }
  bool takeJoin(Credentials& out){
    if(!joinPending_||phase_!=SetupPhase::Joining)return false;
    out=candidate_;joinPending_=false;return true;
  }
  void service(uint32_t now,bool connectedToCandidate){
    if(!active(now)){cancel();return;}
    if(phase_!=SetupPhase::Joining)return;
    if(uint32_t(now-joinAt_)>=30000){phase_=SetupPhase::Failed;clearPassword();}
    else if(connectedToCandidate){phase_=SetupPhase::SaveRequested;joinPending_=false;}
  }
  bool persisted(bool success){
    if(phase_!=SetupPhase::SaveRequested)return false;
    phase_=success?SetupPhase::Success:SetupPhase::Failed;clearPassword();return true;
  }
  void cancel(){
    phase_=SetupPhase::Closed;joinPending_=false;
    volatile char* p=reinterpret_cast<volatile char*>(&candidate_);
    for(size_t n=0;n<sizeof candidate_;++n)p[n]=0;
    for(auto& c:token_)c=0;
  }
 private:
  void clearPassword(){volatile char* p=candidate_.password;for(size_t n=0;n<sizeof candidate_.password;++n)p[n]=0;joinPending_=false;}
  SetupPhase phase_=SetupPhase::Closed;Credentials candidate_{};
  char token_[33]{};uint32_t opened_=0,joinAt_=0;bool joinPending_=false;
};
}
