#ifndef DSPI_PANEL_INPUT_MENU_H
#define DSPI_PANEL_INPUT_MENU_H
#include <stdint.h>
// ESP-only presentation. Native source IDs, hardware enable and routing never change.
struct PanelInputMenu {
 uint8_t visible=0x7f,order[7]={0,1,2,3,4,5,6};
 bool valid()const{uint8_t seen=0;for(auto s:order){if(s>=7||(seen&(1u<<s)))return false;seen|=1u<<s;}return seen==0x7f&&visible&&!(visible&0x80);}
 uint8_t mask(uint8_t available)const{return visible&available&0x7f;}
 bool setVisible(uint8_t source,bool show,uint8_t available){if(source>=7)return false;uint8_t next=show?visible|(1u<<source):visible&~(1u<<source);if(!(next&available&0x7f))return false;visible=next;return true;}
 uint8_t count(uint8_t available)const{uint8_t n=0;for(auto s:order)n+=!!(mask(available)&(1u<<s));return n;}
 uint8_t position(uint8_t source,uint8_t available)const{uint8_t rank=0;for(auto s:order)if(mask(available)&(1u<<s)){++rank;if(s==source)return rank;}return 0;}
 bool moveTo(uint8_t source,uint8_t rank,uint8_t available){
  uint8_t current=position(source,available);if(!current||!rank||rank>count(available))return false;
  int i=0;while(i<7&&order[i]!=source)++i;
  while(current!=rank){int d=current>rank?-1:1,j=i+d;while(j>=0&&j<7&&!(mask(available)&(1u<<order[j])))j+=d;if(j<0||j>=7)return false;uint8_t t=order[i];order[i]=order[j];order[j]=t;i=j;current+=d;}return true;
 }
 uint8_t next(uint8_t source,int direction,uint8_t available)const{
  uint8_t selectable=mask(available);if(!selectable)selectable=available&0x7f;if(!selectable)return source<7?source:0;
  int at=0;while(at<7&&order[at]!=source)++at;if(at==7)at=direction>=0?6:0;
  for(unsigned n=0;n<7;++n){at=(at+(direction>=0?1:6))%7;if(selectable&(1u<<order[at]))return order[at];}return source;
 }
};
#endif
