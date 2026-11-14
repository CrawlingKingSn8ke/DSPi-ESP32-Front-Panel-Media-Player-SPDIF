#pragma once
#include <cstdint>
#include <cstddef>
#include <algorithm>
namespace NetworkPlayback {
// Block briefly after bounded CPU work; taskYIELD alone can reschedule the same
// priority-3 task ahead of the UI/idle tasks.
class WorkBudget {
 public:void reset(uint32_t now){at_=now;}bool due(uint32_t now)const{return uint32_t(now-at_)>=4000;}
 private:uint32_t at_=0;
};
inline size_t prefill(uint32_t rate,size_t capacity){return std::min<size_t>(uint64_t(rate)*2,capacity*3/4);}
inline bool backgroundPressure(size_t capacity,size_t available){return capacity&&uint64_t(available)*100<uint64_t(capacity)*40;}
class BackgroundGate {
 public:
  bool observe(uint32_t generation,bool playing,bool paused,uint32_t now){
   const bool changed=!seen_||generation!=generation_;
   if(changed||!playing||!playing_)stableAt_=now;
   seen_=true;generation_=generation;playing_=playing;paused_=paused;return changed;
  }
  bool allowed(size_t capacity,size_t available,uint32_t now)const{
   return seen_&&(paused_||(playing_&&uint32_t(now-stableAt_)>=1500&&capacity&&
       uint64_t(available)*100>=uint64_t(capacity)*80));
  }
 private:uint32_t generation_=0,stableAt_=0;bool seen_=false,playing_=false,paused_=false;
};
}
