#ifndef DSPI_PANEL_NUMBER_EDITOR_H
#define DSPI_PANEL_NUMBER_EDITOR_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
// Integer decimal ticks prevent repeated 0.1/0.01 adjustments accumulating error.
// No mutation is emitted by this editor; Apply belongs to the native controller.
struct PanelNumberEditor {
  int32_t ticks=0,minimum=0,maximum=0,scale=1;
  uint8_t decimals=1,place=0,maxPlace=0,integerDigits=1;
  float original=0;bool dirty=false;
  bool begin(float v,float low,float high,uint8_t dp){
    if(!isfinite(v)||!isfinite(low)||!isfinite(high)||dp>2||low>high||v<low||v>high||fabsf(low)>100000||fabsf(high)>100000)return false;
    decimals=dp;scale=dp==2?100:dp==1?10:1;original=v;dirty=false;
    ticks=lroundf(v*scale);minimum=ceilf(low*scale);maximum=floorf(high*scale);
    if(minimum>maximum)return false;
    int32_t bound=maximum>-minimum?maximum:-minimum;maxPlace=0;
    while(bound>=10){bound/=10;++maxPlace;}
    integerDigits=maxPlace>=decimals?maxPlace-decimals+1:1;place=decimals;return true;
  }
  void move(int d){int p=int(place)+d;place=p<0?0:p>maxPlace?maxPlace:p;}
  int32_t step()const{int32_t s=1;for(uint8_t i=0;i<place;++i)s*=10;return s;}
  void adjust(int d){int32_t next=ticks+(d>0?step():-step());if(next<minimum)next=minimum;if(next>maximum)next=maximum;if(next!=ticks){ticks=next;dirty=true;}}
  float value()const{return dirty?float(ticks)/scale:original;}
  void format(char *out,size_t capacity)const{
    int32_t magnitude=ticks<0?-ticks:ticks;
    if(decimals)snprintf(out,capacity,"%s%0*ld.%0*ld",ticks<0?"-":"",integerDigits,long(magnitude/scale),decimals,long(magnitude%scale));
    else snprintf(out,capacity,"%s%0*ld",ticks<0?"-":"",integerDigits,long(magnitude));
  }
  uint8_t cursor(const char *text)const{unsigned seen=0;for(int i=int(strlen(text))-1;i>=0;--i)if(text[i]>='0'&&text[i]<='9'){if(seen++==place)return i;}return 0;}
};
#endif
