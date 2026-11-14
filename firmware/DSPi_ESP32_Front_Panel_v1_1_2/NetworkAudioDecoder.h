#pragma once
#include "MediaByteSource.h"
#include "third_party/dr_libs/dr_wav.h"
#include "third_party/dr_libs/dr_flac.h"
#include "third_party/dr_libs/dr_mp3.h"

struct NetworkAudioInfo {
  enum class Format:uint8_t {Unknown,Wav,Flac,Mp3};
  Format format=Format::Unknown;
  uint32_t rate=0;uint8_t channels=0,bits=0;uint64_t frames=0;
};

// Worker-only decoder. Uses the exact dr_libs functions already linked by
// MediaPlayerPoC, not another decoder library or output pipeline.
class NetworkAudioDecoder {
public:
  ~NetworkAudioDecoder(){close();}
  bool open(MediaByteSource&,void(*idle)());
  void close();
  size_t decode(int32_t* stereo,size_t maxFrames);
  bool seekFrame(uint64_t);
  const NetworkAudioInfo& info()const{return info_;}
  bool failed()const{return fault_;}
private:
  static size_t readCallback(void*,void*,size_t);
  static drwav_bool32 wavSeek(void*,int,drwav_seek_origin);
  static drflac_bool32 flacSeek(void*,int,drflac_seek_origin);
  static drmp3_bool32 mp3Seek(void*,int,drmp3_seek_origin);
  static drwav_bool32 tellCallback(void*,drwav_int64*);
  bool seekOffset(int,int);
  MediaByteSource* source_=nullptr;void(*idle_)()=nullptr;
  NetworkAudioInfo info_;bool fault_=false,wavOpen_=false,mp3Open_=false;
  drwav wav_{};drflac* flac_=nullptr;drmp3 mp3_{};
  drwav_int32 scratch_[1024];drmp3_int16 mp3Scratch_[1024];
};
