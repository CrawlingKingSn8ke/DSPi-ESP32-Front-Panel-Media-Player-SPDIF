#pragma once
#include <cstdint>
namespace PanelWifiGlyph {
template<class Pixel>void draw(Pixel pixel,int x,int y,uint16_t colour){
 // Reference silhouette, sampled once to the existing 20px home-header slot.
 // Three rounded upper arcs plus a circular dot; no run-time trig or allocation.
 static constexpr uint32_t rows[]={16320,65520,129528,491550,986895,950247,
   65520,123000,229404,73608,16320,28896,24672,3840,3840,3840,3840,1536};
 for(int py=0;py<18;++py)for(int px=0;px<20;++px)
  if(rows[py]&(1u<<px))pixel(x+px,y+py+1,colour);
}
}
