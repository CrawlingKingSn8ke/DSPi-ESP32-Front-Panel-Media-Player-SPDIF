#include "NetworkArtworkJpeg.h"
#include <jpeg_decoder.h>
#include <esp_heap_caps.h>
#include <cstdlib>
#include <initializer_list>
bool decodeNetworkArtwork(const uint8_t* jpeg,size_t length,uint16_t* pixels,const std::atomic<bool>& cancelled){
 if(cancelled||!jpeg||!length||!pixels)return false;
 esp_jpeg_image_cfg_t config{};config.indata=const_cast<uint8_t*>(jpeg);config.indata_size=length;
 config.out_format=JPEG_IMAGE_FORMAT_RGB565;config.flags.swap_color_bytes=0;
 config.out_scale=JPEG_IMAGE_SCALE_0;esp_jpeg_image_output_t full{};
 if(esp_jpeg_get_image_info(&config,&full)!=ESP_OK||!full.width||!full.height||full.width>2048||full.height>2048)return false;
 esp_jpeg_image_output_t info{};
 for(auto scale:{JPEG_IMAGE_SCALE_1_8,JPEG_IMAGE_SCALE_1_4,JPEG_IMAGE_SCALE_1_2,JPEG_IMAGE_SCALE_0}){
  config.out_scale=scale;if(esp_jpeg_get_image_info(&config,&info)!=ESP_OK)return false;
  if(info.width>=112&&info.height>=112)break;
 }
 if(cancelled||!info.width||!info.height||!info.output_len||info.output_len>256*1024)return false;
 auto* decoded=static_cast<uint8_t*>(heap_caps_malloc(info.output_len,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
 if(!decoded)return false;config.outbuf=decoded;config.outbuf_size=info.output_len;esp_jpeg_image_output_t result{};
 bool okay=!cancelled&&esp_jpeg_decode(&config,&result)==ESP_OK&&result.width&&result.height&&uint64_t(result.width)*result.height*2<=info.output_len;
 if(okay){
  const auto* image=reinterpret_cast<const uint16_t*>(decoded);
  unsigned xs[112];for(unsigned x=0;x<112;++x)xs[x]=uint32_t(x)*result.width/112;
  for(unsigned y=0;y<112&&!cancelled;++y){const unsigned row=uint32_t(y)*result.height/112;for(unsigned x=0;x<112;++x)pixels[y*112+x]=image[row*result.width+xs[x]];}
 }
 free(decoded);return okay&&!cancelled;
}
