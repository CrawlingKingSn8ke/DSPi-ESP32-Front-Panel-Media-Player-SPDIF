#pragma once
#include "NetworkAudioDecoder.h"
#include "NetworkPcmSink.h"
namespace NetworkPlayer {
enum class OutputState:uint8_t {Stopped,Starting,Playing,Paused,Finished,Error,Stopping};
struct OutputSnapshot {OutputState state=OutputState::Stopped;uint64_t frames=0;};
class Output:public NetworkPcmSink {
public:
 virtual void stop(bool retainCarrier)=0;
 virtual bool cleanup()=0;
 virtual bool begin(MediaByteSource&,const NetworkAudioInfo&)=0;
 virtual int start()=0;
 virtual uint32_t pcmToken()const=0;
 virtual OutputSnapshot snapshot()const=0;
 virtual bool pause(bool)=0;
 virtual bool sourceMatches()const=0;
 virtual bool setVolume(uint8_t)=0;
 virtual bool setMute(bool)=0;
 virtual void volume(float& db,bool& muted)const=0;
};
}
