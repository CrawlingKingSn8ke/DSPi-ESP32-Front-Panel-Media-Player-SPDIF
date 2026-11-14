#pragma once
#ifndef DSPI_AUDIO_DIAGNOSTICS_H
#define DSPI_AUDIO_DIAGNOSTICS_H
#include <cstddef>
#include <cstdint>
namespace AudioDiagnostics {
enum class Stage:uint8_t {None,PcmRing,DecoderTask,OutputTask,OutputStaging};
struct Failure {Stage stage=Stage::None;uint32_t required=0,free=0,largest=0,psramFree=0,psramLargest=0,count=0;};
struct NetworkSample {
 uint32_t rate=0,bits=0,buffered=0,capacity=0,underruns=0,readCalls=0,readMaxUs=0,decodeCalls=0,decodeMaxUs=0,computeMaxUs=0;
 uint64_t readBytes=0,readUs=0,decodedFrames=0;
 uint64_t pollUs=0,serviceUs=0,windowPollUs=0,windowServiceUs=0;
 uint32_t pollCalls=0,pollTimeouts=0,serviceMaxUs=0,readErrors=0,readOver100ms=0,readOver250ms=0,readOver500ms=0;
 uint32_t generation=0,trackHash=0,elapsedMs=0,outputErrors=0,underrunFrames=0;
 uint32_t compressedReady=0,compressedCapacity=0;
 uint64_t outputFrames=0;
 uint32_t windowMs=0,windowDecodedFrames=0,windowOutputFrames=0;uint64_t windowReadUs=0;
 uint32_t freeInternal=0,largestInternal=0,minimumInternal=0,minimumLargest=0,freePsram=0;
 int32_t rssi=-127,sleep=-1;uint8_t phase=0,jobs=0;
 char track[97]={0}; // basename only; never credentials or the full share path
};
enum class Event:uint8_t {None,LinkStarted,LinkStopping,LinkRestart,NetworkStarted,NetworkRestart};
struct ModeEvent{Event kind=Event::None;uint32_t at=0;};
struct Snapshot{Failure failure;NetworkSample network,lastUnderrun;ModeEvent events[8];};
void allocationFailure(const char* reason,uint32_t free,uint32_t largest,uint32_t psramFree,uint32_t psramLargest);
void networkSample(const NetworkSample&);
void event(Event,uint32_t uptimeMs);
Snapshot snapshot();
void preserveForRestart();
bool formatJson(char*,size_t,int resetReason);
}
#endif
