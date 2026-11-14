#pragma once
#include <stdint.h>
#include <math.h>
namespace DspiLink {
enum class ControlAttempt:uint8_t {Idle,Ok,Busy,Failed,Expired};
inline bool controlWrite(uint8_t req){return req==0xd2||req==0xda||req==0xdc;}
inline float relativeVolumeTarget(float current,float delta,float minimumDb=-60.0f,float muteDb=-60.0f){
  // Master reserves a separate mute sentinel below its visible -60..0 scale.
  if(current<minimumDb)return delta>0 ? minimumDb : muteDb;
  float target=roundf((current+delta)*2)/2;
  if(target>0)target=0;
  if(target<minimumDb)target=muteDb;
  return target;
}
class RelativeVolumeQueue {
 public:
  void clear(){pending_=false;delta_=0;}
  void add(float delta,uint32_t context,uint32_t now){
    if(!isfinite(delta))return;
    if(!pending_||context!=context_){delta_=0;context_=context;until_=now+750;}
    delta_+=delta;if(delta_>60)delta_=60;if(delta_<-60)delta_=-60;
    pending_=true; // Further detents never prolong the original deadline.
  }
  template<class Clock,class Read,class Write> ControlAttempt service(uint32_t context,Clock clock,Read read,Write write,float minimumDb=-60.0f,float muteDb=-60.0f){
    if(!pending_)return ControlAttempt::Idle;
    if(context!=context_){clear();return ControlAttempt::Idle;}
    if(int32_t(clock()-until_)>=0){clear();return ControlAttempt::Expired;}
    float current=0;auto result=read(current);
    if(result==ControlAttempt::Busy)return result;
    if(result!=ControlAttempt::Ok||!isfinite(current)){clear();return ControlAttempt::Failed;}
    // The fresh read can block on the UART; never send a late volume write.
    if(int32_t(clock()-until_)>=0){clear();return ControlAttempt::Expired;}
    float target=relativeVolumeTarget(current,delta_,minimumDb,muteDb);
    result=write(target);
    if(result!=ControlAttempt::Busy)clear(); // Unknown writes are never replayed.
    return result;
  }
 private:bool pending_=false;float delta_=0;uint32_t context_=0,until_=0;
};
template<class T,class Read> bool freshLocalValue(bool shared,T& cached,Read read){
  if(!shared)return true;
  T confirmed{};if(!read(confirmed))return false;
  cached=confirmed;return true;
}
}
