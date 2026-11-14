#include "AudioDiagnostics.h"
#include "NetworkRecovery.h"
#ifdef ESP_PLATFORM
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_app_desc.h>
#endif
#include <mutex>
#include <cstring>
#include <cstdio>
#include <cstdarg>
namespace AudioDiagnostics {
namespace{std::mutex mutex;Snapshot data;bool networkSeen=false;
#ifdef ESP_PLATFORM
// Raw bytes avoid a C++ global constructor clearing the retained sample.
RTC_NOINIT_ATTR unsigned char retained[sizeof(NetworkRecovery::Record)];
bool restored=false;
void restore(){if(!restored){restored=true;NetworkRecovery::Record record;memcpy(&record,retained,sizeof record);NetworkRecovery::take(record,esp_reset_reason()==ESP_RST_SW,data);memset(retained,0,sizeof retained);}}
#else
void restore(){}
#endif
const char* label(Stage s){switch(s){case Stage::PcmRing:return "pcm_ring";case Stage::DecoderTask:return "decoder_task";case Stage::OutputTask:return "output_task";case Stage::OutputStaging:return "output_staging";default:return "none";}}
}
void allocationFailure(const char* reason,uint32_t free,uint32_t largest,uint32_t psramFree,uint32_t psramLargest){
 if(!reason)return;Stage stage=Stage::None;uint32_t required=0;
 if(!strcmp(reason,"PSRAM ring allocation failed"))stage=Stage::PcmRing;
 else if(!strcmp(reason,"decoder task creation failed")){stage=Stage::DecoderTask;required=32768;}
 else if(!strcmp(reason,"S/PDIF output task creation failed")){stage=Stage::OutputTask;required=8192;}
 else if(!strcmp(reason,"S/PDIF DMA staging allocation failed"))stage=Stage::OutputStaging;
 if(stage==Stage::None)return;std::lock_guard<std::mutex> lock(mutex);restore();auto count=data.failure.count+1;data.failure={stage,required,free,largest,psramFree,psramLargest,count};
}
void networkSample(const NetworkSample& sample){
 if(!sample.rate)return;std::lock_guard<std::mutex> lock(mutex);restore();
 auto next=sample;const auto& previous=data.network;
 const bool same=networkSeen&&previous.rate&&next.generation==previous.generation&&next.trackHash==previous.trackHash;networkSeen=true;
 next.windowMs=0;next.windowReadUs=0;next.windowPollUs=next.windowServiceUs=0;next.windowDecodedFrames=next.windowOutputFrames=0;
 if(same&&next.elapsedMs>=previous.elapsedMs&&next.readUs>=previous.readUs&&next.decodedFrames>=previous.decodedFrames&&next.outputFrames>=previous.outputFrames){
  next.windowMs=next.elapsedMs-previous.elapsedMs;next.windowReadUs=next.readUs-previous.readUs;
  if(next.pollUs>=previous.pollUs)next.windowPollUs=next.pollUs-previous.pollUs;
  if(next.serviceUs>=previous.serviceUs)next.windowServiceUs=next.serviceUs-previous.serviceUs;
  next.windowDecodedFrames=uint32_t(next.decodedFrames-previous.decodedFrames);next.windowOutputFrames=uint32_t(next.outputFrames-previous.outputFrames);
 }
 if(next.underruns>(same?previous.underruns:0))data.lastUnderrun=next;
 data.network=next;
}
void event(Event kind,uint32_t at){
 std::lock_guard<std::mutex> lock(mutex);restore();
 unsigned slot=0;while(slot<8&&data.events[slot].kind!=Event::None)++slot;
 if(slot==8){for(unsigned i=1;i<8;++i)data.events[i-1]=data.events[i];slot=7;}
 data.events[slot]={kind,at};
}
Snapshot snapshot(){std::lock_guard<std::mutex> lock(mutex);restore();return data;}
void preserveForRestart(){
#ifdef ESP_PLATFORM
 std::lock_guard<std::mutex> lock(mutex);restore();NetworkRecovery::Record record{};NetworkRecovery::save(record,data);memcpy(retained,&record,sizeof record);
#endif
}
namespace {
struct Json {
 char* out;size_t capacity,used=0;bool okay=true;
 void add(const char* format,...){if(!okay)return;va_list args;va_start(args,format);int n=vsnprintf(out+used,capacity-used,format,args);va_end(args);
  if(n<0||size_t(n)>=capacity-used){okay=false;return;}used+=size_t(n);}
 void sample(const NetworkSample& n){
  char track[sizeof n.track];for(size_t i=0;i<sizeof track;++i){const unsigned char c=n.track[i];track[i]=c>=32&&c<127&&c!=34&&c!=92?char(c):'_';if(!c){track[i]=0;break;}}track[sizeof track-1]=0;
  add("{\"sampleRate\":%u,\"bits\":%u,\"bufferedFrames\":%u,\"capacityFrames\":%u,\"underruns\":%u,\"readCalls\":%u,\"readBytes\":%llu,\"readMicroseconds\":%llu,\"maxReadMicroseconds\":%u,\"decodeCalls\":%u,\"maxDecodeMicroseconds\":%u,\"maxDecodeExcludingReadMicroseconds\":%u,\"decodedFrames\":%llu,",
    unsigned(n.rate),unsigned(n.bits),unsigned(n.buffered),unsigned(n.capacity),unsigned(n.underruns),unsigned(n.readCalls),(unsigned long long)n.readBytes,(unsigned long long)n.readUs,unsigned(n.readMaxUs),unsigned(n.decodeCalls),unsigned(n.decodeMaxUs),unsigned(n.computeMaxUs),(unsigned long long)n.decodedFrames);
  add("\"generation\":%u,\"trackHash\":%u,\"track\":\"%s\",\"elapsedMs\":%u,\"outputFrames\":%llu,\"outputErrors\":%u,\"underrunFrames\":%u,\"windowMs\":%u,\"windowReadUs\":%llu,\"windowDecodedFrames\":%u,\"windowOutputFrames\":%u,",
    unsigned(n.generation),unsigned(n.trackHash),track,unsigned(n.elapsedMs),(unsigned long long)n.outputFrames,unsigned(n.outputErrors),unsigned(n.underrunFrames),unsigned(n.windowMs),(unsigned long long)n.windowReadUs,unsigned(n.windowDecodedFrames),unsigned(n.windowOutputFrames));
  add("\"socketWaitMicroseconds\":%llu,\"protocolServiceMicroseconds\":%llu,\"maxProtocolServiceMicroseconds\":%u,\"socketPollCalls\":%u,\"socketPollTimeouts\":%u,\"readErrors\":%u,\"readsOver100ms\":%u,\"readsOver250ms\":%u,\"readsOver500ms\":%u,\"windowSocketWaitUs\":%llu,\"windowProtocolServiceUs\":%llu,",
    (unsigned long long)n.pollUs,(unsigned long long)n.serviceUs,unsigned(n.serviceMaxUs),unsigned(n.pollCalls),unsigned(n.pollTimeouts),unsigned(n.readErrors),unsigned(n.readOver100ms),unsigned(n.readOver250ms),unsigned(n.readOver500ms),(unsigned long long)n.windowPollUs,(unsigned long long)n.windowServiceUs);
  add("\"freeInternal\":%u,\"largestInternal\":%u,\"minimumInternal\":%u,\"minimumLargest\":%u,\"freePsram\":%u,\"rssiDbm\":%d,\"wifiSleep\":%d,\"phase\":%u,\"jobs\":%u}",
    unsigned(n.freeInternal),unsigned(n.largestInternal),unsigned(n.minimumInternal),unsigned(n.minimumLargest),unsigned(n.freePsram),int(n.rssi),int(n.sleep),unsigned(n.phase),unsigned(n.jobs));
 }
};
}
bool formatJson(char* output,size_t capacity,int resetReason){
 if(!output||!capacity)return false;const auto s=snapshot();const auto& f=s.failure;const auto& n=s.network;
 char sha[65]="host-test";
#ifdef ESP_PLATFORM
 const auto* app=esp_app_get_description();for(unsigned i=0;i<32;++i)snprintf(sha+i*2,3,"%02x",app->app_elf_sha256[i]);
#endif
 Json json{output,capacity};json.add("{\"runningBuild\":\"v1.5.0\",\"runningElfSha256\":\"%s\",\"bootResetReason\":%d,\"allocation\":{\"stage\":\"%s\",\"requiredBytes\":%u,\"freeInternal\":%u,\"largestInternal\":%u,\"freePsram\":%u,\"largestPsram\":%u,\"failures\":%u},\"lastNetworkPlayback\":",
 sha,resetReason,label(f.stage),unsigned(f.required),unsigned(f.free),unsigned(f.largest),unsigned(f.psramFree),unsigned(f.psramLargest),unsigned(f.count));json.sample(n);
 json.add(",\"lastUnderrun\":");if(s.lastUnderrun.rate)json.sample(s.lastUnderrun);else json.add("null");
 json.add(",\"modeEvents\":[");for(unsigned i=0;i<8&&s.events[i].kind!=Event::None;++i)json.add("%s{\"kind\":%u,\"uptimeMs\":%u}",i?",":"",unsigned(s.events[i].kind),unsigned(s.events[i].at));json.add("]}");return json.okay;
}
}
