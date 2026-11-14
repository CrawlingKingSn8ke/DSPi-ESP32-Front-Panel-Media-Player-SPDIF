#pragma once
#include <cstdint>
namespace WifiSignal {
inline unsigned bars(int dbm){return dbm>=-55?5:dbm>=-65?4:dbm>=-75?3:dbm>=-85?2:1;}
inline unsigned menuCount(bool connected){return connected?5:4;}
inline bool isSignal(unsigned row,bool connected){return connected&&row==3;}
inline unsigned baseRow(unsigned row,bool connected){return connected&&row>3?row-1:row;}
class Sampler {
 bool sampled_=false;uint32_t at_=0;
public:
 bool due(uint32_t now,bool visible,bool connected){
  if(!visible||!connected){sampled_=false;return false;}
  if(sampled_&&uint32_t(now-at_)<1000)return false;
  sampled_=true;at_=now;return true;
 }
};
// Native pixels, same bars for Classic/Retro. Colours supplied by theme roles.
template<class Rect>void draw(Rect rect,int x,int baseline,unsigned active,uint16_t on,uint16_t off){
 for(unsigned i=0;i<5;++i){int h=18+int(i)*18;rect(x+int(i)*34,baseline-h,24,h,i<active?on:off);}
}
}
