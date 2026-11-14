#pragma once
#include <stdint.h>
#include <stddef.h>
#include "RetroOledFont.h"
#include "RetroSegmentFont.h"

// Allocation-free pixel geometry shared by the panel and host rendering tests.
namespace RetroPixel {
struct Settings { uint8_t version, enabled, text, accent, meters, volume, unlit; };
inline Settings defaults() { return {1,0,8,8,8,0,1}; }
inline bool valid(const Settings &s,uint8_t colours) {
  return s.version==1 && s.enabled<=1 && s.text<colours && s.accent>=3 &&
      s.accent<colours && s.meters<colours && s.volume<colours && s.unlit<=3;
}
inline uint16_t dim(uint16_t c,uint8_t a) {
  return uint16_t(((((c>>11)&31)*a/255)<<11) |
      ((((c>>5)&63)*a/255)<<5) | ((c&31)*a/255));
}
inline uint8_t unlitAlpha(uint8_t /*legacyChoice*/) {
  // Keep the seven-byte saved record compatible; rendering always uses Low.
  return 38;
}
inline void glyph(char c,uint8_t (&out)[5]) {
  const unsigned ch=static_cast<unsigned char>(c);
  const unsigned index=(ch>=32&&ch<=126?ch:unsigned('?'))-32;
  for(int i=0;i<5;i++)out[i]=RetroFonts::oled[index][i];
}
inline int width(const char *s,int pitch,int advance=0) {
  int n=0;if(advance<=0)advance=6*pitch;while(*s++)n+=advance;return n;
}
template<class Pixel> void text(Pixel pixel,int x,int y,const char *s,int pitch,
    int top,uint16_t on,uint16_t off,int scale=100,
    int left=0,int upper=0,int right=320,int bottom=240,int xPitch=0,
    bool unitCase=false,int advance=0) {
  (void)unitCase; // Full native glyph set now preserves case for every string.
  if(scale<=0||pitch<=0)return;
  if(xPitch<=0)xPitch=pitch;
  if(advance<=0)advance=6*xPitch;
  int cursor=0;
  for(;*s;s++,cursor+=advance) {
    uint8_t bits[5];glyph(*s,bits);
    for(int col=0;col<5;col++)for(int row=0;row<8;row++) {
      const bool active=bits[col]&(1<<row);
      const uint16_t colour=active?on:off;if(!colour)continue;
      const int gap=active?0:1;
      const int px=x+((cursor+col*xPitch)*scale+50)/100;
      const int py=y+((top+row*pitch)*scale+50)/100;
      const int ex=x+((cursor+col*xPitch+xPitch-gap)*scale+50)/100;
      const int ey=y+((top+row*pitch+pitch-gap)*scale+50)/100;
      for(int yy=py;yy<ey;yy++)for(int xx=px;xx<ex;xx++)
        if(xx>=left&&xx<right&&yy>=upper&&yy<bottom)pixel(xx,yy,colour);
    }
  }
}
template<class Pixel> void segment(Pixel pixel,int x,int y,unsigned glyphIndex,
    int w,int h,uint16_t colour) {
  if(!colour||glyphIndex>=12||w<=0||h<=0)return;
  for(int row=0;row<h;row++)for(int col=0;col<w;col++) {
    const int sx=col*RetroFonts::segmentWidth/w;
    const int sy=row*RetroFonts::segmentHeight/h;
    const uint8_t value=RetroFonts::segment[glyphIndex][sy*RetroFonts::segmentStride+sx/8];
    const int px=x+col,py=y+row;
    if((value&(1u<<(sx%8)))&&px>=0&&px<320&&py>=0&&py<240)pixel(px,py,colour);
  }
}
inline int numberAdvance(char c,int w) {return c=='.'?0:w;}
inline int numberWidth(const char *s,int w) {int n=0;while(*s)n+=numberAdvance(*s++,w);return n;}
template<class Pixel> void number(Pixel pixel,int x,int y,const char *s,int w,
    int h,uint16_t on,uint16_t off) {
  for(;*s;s++) {
    if(*s=='.'){segment(pixel,x-w,y,11,w,h,on);continue;}
    segment(pixel,x,y,8,w,h,off);
    if(*s>='0'&&*s<='9')segment(pixel,x,y,unsigned(*s-'0'),w,h,on);
    else if(*s=='-')segment(pixel,x,y,10,w,h,on);
    x+=w;
  }
}
inline int volumeWidth(int w) {return 4*w;}
template<class Pixel> void volume(Pixel pixel,int x,int y,const char *s,int w,
    int h,uint16_t on,uint16_t off) {
  const bool negative=*s=='-';if(negative||*s=='+')++s;
  const char *dot=s;while(*dot&&*dot!='.')++dot;
  const int count=static_cast<int>(dot-s);
  if(count<1||count>3)return;
  const int signSlot=negative&&count<3?2-count:-1;
  for(int slot=0;slot<3;slot++) {
    segment(pixel,x+slot*w,y,8,w,h,off);
    const int index=slot-(3-count);
    if(slot==signSlot)segment(pixel,x+slot*w,y,10,w,h,on);
    else if(index>=0&&s[index]>='0'&&s[index]<='9')
      segment(pixel,x+slot*w,y,unsigned(s[index]-'0'),w,h,on);
  }
  // Only the rare three-integer-digit values need a compact external sign.
  // Do not reserve a fifth cell or shift the ordinary volume field for them.
  if(negative&&count==3)segment(pixel,x-w/4,y+h/4,10,w/4,h/2,on);
  segment(pixel,x+2*w,y,11,w,h,on);
  segment(pixel,x+3*w,y,8,w,h,off);
  const char fractional=*dot=='.'&&dot[1]?dot[1]:'0';
  if(fractional>='0'&&fractional<='9')
    segment(pixel,x+3*w,y,unsigned(fractional-'0'),w,h,on);
}
} // namespace RetroPixel
