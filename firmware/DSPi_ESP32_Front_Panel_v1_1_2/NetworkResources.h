#pragma once
#include <cstdint>
#include <cstddef>
#include <algorithm>
class NetworkResources {
public:
 bool observe(uint32_t now,size_t internal,size_t largest,size_t psram,bool active){
  if(!active_&&!active)return false;
  if(active&&!active_){internal_=internal;largest_=largest;psram_=psram;}
  else {internal_=std::min(internal_,internal);largest_=std::min(largest_,largest);psram_=std::min(psram_,psram);}
  bool report=active!=active_||uint32_t(now-at_)>=10000;active_=active;
  if(report)at_=now;return report;
 }
 size_t minimumInternal()const{return internal_;}size_t minimumLargest()const{return largest_;}size_t minimumPsram()const{return psram_;}
private:size_t internal_=0,largest_=0,psram_=0;uint32_t at_=0;bool active_=false;
};
