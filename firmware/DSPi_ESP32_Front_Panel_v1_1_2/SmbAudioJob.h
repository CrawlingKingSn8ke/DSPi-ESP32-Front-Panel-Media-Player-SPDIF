#pragma once
#include "SmbClient.h"
#include "SmbByteSource.h"
#include "NetworkAudioDecoder.h"
#include "NetworkPcmSink.h"
namespace SmbLibrary {
struct AudioTelemetry {ReadStatistics reads;uint32_t decodeCalls=0,decodeMaxUs=0,computeMaxUs=0;uint64_t decodedFrames=0;uint32_t compressedReady=0,compressedCapacity=0;};
class AudioJob {
public:
 ~AudioJob();
 bool start(const Credentials&,const char* path,uint32_t generation,NetworkPcmSink&,
            uint64_t firstFrame=0,const Identity* expected=nullptr);
 bool ready()const;bool failed()const;bool stopped()const;
 NetworkAudioInfo info()const;Identity identity()const;uint32_t generation()const;
 MediaByteSource* source()const;const char* error()const;
 void supplyPcmToken(uint32_t);void cancel();bool release();
 AudioTelemetry telemetry()const;
private:
 struct State;State* state_=nullptr;static void run(void*);
};
}
