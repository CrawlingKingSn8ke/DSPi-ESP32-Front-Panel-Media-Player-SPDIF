#include "NetworkAudioDecoder.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <climits>
#ifdef ARDUINO_ARCH_ESP32
#include <esp_heap_caps.h>
#endif
namespace {
void* audioAlloc(size_t n,void*){
#ifdef ARDUINO_ARCH_ESP32
 return heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
 return malloc(n);
#endif
}
void* audioRealloc(void* p,size_t n,void*){
#ifdef ARDUINO_ARCH_ESP32
 return heap_caps_realloc(p,n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
 return realloc(p,n);
#endif
}
void audioFree(void* p,void*){free(p);}
}
size_t NetworkAudioDecoder::readCallback(void* context,void* output,size_t wanted){
 auto& d=*static_cast<NetworkAudioDecoder*>(context);size_t total=0;
 while(total<wanted){
  const auto r=d.source_->read(static_cast<uint8_t*>(output)+total,wanted-total);
  if(r.count>wanted-total){d.fault_=true;break;}
  if(r.status==MediaReadResult::Status::Data){if(!r.count){d.fault_=true;break;}total+=r.count;}
  else if(r.status==MediaReadResult::Status::Waiting){if(d.idle_)d.idle_();else{d.fault_=true;break;}}
  else{if(r.status==MediaReadResult::Status::Error)d.fault_=true;break;}
 }
 return total;
}
bool NetworkAudioDecoder::seekOffset(int offset,int origin){
 uint64_t base=0;
 if(origin==1)base=source_->tell();
 else if(origin==2 && !source_->length(base))return false;
 if(offset<0 && base<uint64_t(-int64_t(offset)))return false;
 if(offset>0 && base>UINT64_MAX-uint64_t(offset))return false;
 const uint64_t target=offset<0?base-uint64_t(-int64_t(offset)):base+uint64_t(offset);
 return source_->seek(target);
}
drwav_bool32 NetworkAudioDecoder::wavSeek(void* p,int n,drwav_seek_origin o){return static_cast<NetworkAudioDecoder*>(p)->seekOffset(n,int(o));}
drflac_bool32 NetworkAudioDecoder::flacSeek(void* p,int n,drflac_seek_origin o){return static_cast<NetworkAudioDecoder*>(p)->seekOffset(n,int(o));}
drmp3_bool32 NetworkAudioDecoder::mp3Seek(void* p,int n,drmp3_seek_origin o){return static_cast<NetworkAudioDecoder*>(p)->seekOffset(n,int(o));}
drwav_bool32 NetworkAudioDecoder::tellCallback(void* p,drwav_int64* out){
 const uint64_t n=static_cast<NetworkAudioDecoder*>(p)->source_->tell();if(n>INT64_MAX)return false;*out=(drwav_int64)n;return true;
}
bool NetworkAudioDecoder::open(MediaByteSource& source,void(*idle)()){
 close();source_=&source;idle_=idle;fault_=false;
 uint8_t signature[12];
 if(readCallback(this,signature,sizeof(signature))!=sizeof(signature) || !source.seek(0))return false;
 if(!memcmp(signature,"RIFF",4)&&!memcmp(signature+8,"WAVE",4)){
  drwav_allocation_callbacks a{nullptr,audioAlloc,audioRealloc,audioFree};
  if(!drwav_init_ex(&wav_,readCallback,wavSeek,tellCallback,nullptr,this,nullptr,DRWAV_SEQUENTIAL,&a))return false;
  wavOpen_=true;info_={NetworkAudioInfo::Format::Wav,wav_.sampleRate,(uint8_t)wav_.channels,(uint8_t)wav_.bitsPerSample,wav_.totalPCMFrameCount};
  if(wav_.translatedFormatTag!=DR_WAVE_FORMAT_PCM){close();return false;}
 }else if(!memcmp(signature,"fLaC",4)){
  drflac_allocation_callbacks a{nullptr,audioAlloc,audioRealloc,audioFree};
  flac_=drflac_open(readCallback,flacSeek,tellCallback,this,&a);if(!flac_)return false;
  info_={NetworkAudioInfo::Format::Flac,flac_->sampleRate,(uint8_t)flac_->channels,(uint8_t)flac_->bitsPerSample,flac_->totalPCMFrameCount};
 }else if(!memcmp(signature,"ID3",3)||(signature[0]==0xff&&(signature[1]&0xe0)==0xe0)){
  drmp3_allocation_callbacks a{nullptr,audioAlloc,audioRealloc,audioFree};
  if(!drmp3_init(&mp3_,readCallback,mp3Seek,tellCallback,nullptr,this,&a))return false;
  mp3Open_=true;info_={NetworkAudioInfo::Format::Mp3,mp3_.sampleRate,(uint8_t)mp3_.channels,16,0};
 }else return false;
 if(fault_ || (info_.rate!=44100&&info_.rate!=48000)||(info_.channels!=1&&info_.channels!=2)||(info_.bits!=16&&info_.bits!=24)){close();return false;}
 return true;
}
void NetworkAudioDecoder::close(){
 if(wavOpen_)drwav_uninit(&wav_);if(mp3Open_)drmp3_uninit(&mp3_);if(flac_)drflac_close(flac_);
 wavOpen_=mp3Open_=false;flac_=nullptr;source_=nullptr;info_=NetworkAudioInfo{};
}
size_t NetworkAudioDecoder::decode(int32_t* output,size_t maxFrames){
 if(!source_ || !output || fault_)return 0;maxFrames=std::min<size_t>(512,maxFrames);size_t frames=0;
 if(wavOpen_)frames=(size_t)drwav_read_pcm_frames_s32(&wav_,maxFrames,scratch_);
 else if(flac_)frames=(size_t)drflac_read_pcm_frames_s32(flac_,maxFrames,scratch_);
 else if(mp3Open_){
  frames=(size_t)drmp3_read_pcm_frames_s16(&mp3_,maxFrames,mp3Scratch_);
  for(size_t i=0;i<frames*info_.channels;++i)scratch_[i]=int32_t(mp3Scratch_[i])*65536;
 }
 for(size_t i=0;i<frames;++i){output[i*2]=scratch_[i*info_.channels];output[i*2+1]=scratch_[i*info_.channels+(info_.channels==2?1:0)];}
 return frames;
}
bool NetworkAudioDecoder::seekFrame(uint64_t frame){
 if(!source_ || fault_ || !source_->seekable())return false;
 if(wavOpen_)return drwav_seek_to_pcm_frame(&wav_,frame);
 if(flac_)return drflac_seek_to_pcm_frame(flac_,frame);
 return mp3Open_&&drmp3_seek_to_pcm_frame(&mp3_,frame);
}
