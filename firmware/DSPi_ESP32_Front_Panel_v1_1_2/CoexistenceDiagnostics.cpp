#include "CoexistenceDiagnostics.h"
#include <mutex>
#include <new>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <esp_attr.h>
#include <esp_system.h>
#endif
namespace CoexistenceDiagnostics {
namespace {
struct Storage {Capture data;size_t head=0,wifiHead=0;bool seen=false;uint32_t previousGeneration=0,previousHash=0,previousUnderruns=0;};
std::mutex mutex;Storage* storage=nullptr;bool attempted=false;Radio latestRadio;
uint32_t currentBootId=1,wifiSequence=0,wifiDropped=0;int currentResetReason=0;
bool bootInitialized=false,observerRegistered=false;
bool retentionRestoreFailed=false;
#ifdef ESP_PLATFORM
RTC_NOINIT_ATTR unsigned char retainedBytes[sizeof(Retained)];
bool restored=false;
#endif
void* allocate(size_t bytes){
#ifdef ESP_PLATFORM
 return heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
 return malloc(bytes);
#endif
}
bool changed(const Radio& a,const Radio& b){
 return a.connectionEpoch!=b.connectionEpoch||a.connected!=b.connected||a.stackReady!=b.stackReady||
  a.intervalUnits!=b.intervalUnits||a.latency!=b.latency||a.timeoutUnits!=b.timeoutUnits||
  a.softwareScan!=b.softwareScan||a.actualScan!=b.actualScan||a.scanPurpose!=b.scanPurpose||
  a.connecting!=b.connecting||a.queryResult!=b.queryResult||a.encrypted!=b.encrypted||a.bonded!=b.bonded||
  a.stopRequests!=b.stopRequests||a.stopFailures!=b.stopFailures||a.scanEndReason!=b.scanEndReason||
  a.wifiSequence!=b.wifiSequence||a.apQueryResult!=b.apQueryResult||a.apChannel!=b.apChannel||memcmp(a.apBssid,b.apBssid,6);
}
uint32_t checksum(const Retained& r){const auto* bytes=reinterpret_cast<const unsigned char*>(&r);uint32_t h=2166136261u;for(size_t i=8;i<sizeof r;++i)h=(h^bytes[i])*16777619u;return h;}
void restoreOnce(){
#ifdef ESP_PLATFORM
 if(restored)return;restored=true;uint32_t magic=0;memcpy(&magic,retainedBytes,sizeof magic);
 if(magic!=0x424c4502u||esp_reset_reason()!=ESP_RST_SW){memset(retainedBytes,0,sizeof retainedBytes);return;}
 // Restore scratch is PSRAM, not another multi-KiB nested HTTP stack frame.
 void* memory=allocate(sizeof(Retained));if(!memory){retentionRestoreFailed=true;memset(retainedBytes,0,sizeof retainedBytes);return;}
 auto* record=new(memory)Retained;memcpy(record,retainedBytes,sizeof *record);memset(retainedBytes,0,sizeof retainedBytes);
 if(!attempted){attempted=true;if(auto* p=allocate(sizeof(Storage)))storage=new(p)Storage;}
 if(!storage||!restoreRetained(*record,true,storage->data))retentionRestoreFailed=true;
 record->~Retained();free(record);
#endif
}
}
void saveRetained(Retained& out,const Capture& c){
 memset(&out,0,sizeof out);const bool live=c.count!=0;const size_t count=live?c.count:c.retainedCount;
 out.count=uint32_t(std::min<size_t>(8,count));out.beforeCount=uint32_t(std::min<size_t>(8,c.beforeCount));out.hasUnderrun=c.hasUnderrun;
 const auto* rows=live?c.rows:c.retainedHistory;
 for(size_t i=0;i<out.count;++i)out.latest[i]=rows[count-out.count+i];
 for(size_t i=0;i<out.beforeCount;++i)out.beforeUnderrun[i]=c.beforeUnderrun[i];
 const bool liveEvents=c.wifiCount!=0;const size_t eventCount=liveEvents?c.wifiCount:c.retainedWifiCount;const auto* events=liveEvents?c.wifiEvents:c.retainedWifiEvents;
 out.wifiCount=uint32_t(std::min<size_t>(8,eventCount));for(size_t i=0;i<out.wifiCount;++i)out.wifiEvents[i]=events[eventCount-out.wifiCount+i];
 out.underrun=c.underrun;out.checksum=checksum(out);out.magic=0x424c4502u;
}
bool restoreRetained(Retained& r,bool softwareReset,Capture& out){
 const bool valid=softwareReset&&r.magic==0x424c4502u&&r.checksum==checksum(r)&&r.count<=8&&r.beforeCount<=8&&r.wifiCount<=8;r.magic=0;
 if(!valid)return false;
 out.count=out.wifiCount=0;out.retainedCount=r.count;out.retainedWifiCount=r.wifiCount;out.beforeCount=r.beforeCount;out.available=true;out.restoredAfterRestart=true;out.hasUnderrun=r.hasUnderrun;out.underrun=r.underrun;
 for(size_t i=0;i<out.retainedCount;++i)out.retainedHistory[i]=r.latest[i];for(size_t i=0;i<out.beforeCount;++i)out.beforeUnderrun[i]=r.beforeUnderrun[i];
 for(size_t i=0;i<out.retainedWifiCount;++i)out.retainedWifiEvents[i]=r.wifiEvents[i];return true;
}
void preserveForRestart(){
#ifdef ESP_PLATFORM
 auto* c=capture();if(!c)return;Retained r;saveRetained(r,*c);release(c);memcpy(retainedBytes,&r,sizeof r);
#endif
}
void observeRadio(const Radio& value){std::lock_guard<std::mutex> lock(mutex);latestRadio=value;}
void beginBoot(uint32_t id,int resetReason){
 std::lock_guard<std::mutex> lock(mutex);if(bootInitialized)return;bootInitialized=true;currentBootId=id?id:1;currentResetReason=resetReason;
 restoreOnce();if(!attempted){attempted=true;if(auto* p=allocate(sizeof(Storage)))storage=new(p)Storage;}
 if(storage){
  const auto& d=storage->data;
  for(;;){
   bool collision=d.hasUnderrun&&d.underrun.bootId==currentBootId;
   for(size_t i=0;i<d.retainedCount;++i)collision|=d.retainedHistory[i].bootId==currentBootId;
   for(size_t i=0;i<d.retainedWifiCount;++i)collision|=d.retainedWifiEvents[i].bootId==currentBootId;
   for(size_t i=0;i<d.beforeCount;++i)collision|=d.beforeUnderrun[i].bootId==currentBootId;
   if(!collision)break;if(!++currentBootId)currentBootId=1;
  }
 }
}
void wifiObserverRegistration(bool okay){std::lock_guard<std::mutex> lock(mutex);observerRegistered=okay;}
void observeWifiEvent(const WifiEvent& input){
 // Arduino event task: bounded copies only; no allocation, I/O or Wi-Fi calls.
 std::lock_guard<std::mutex> lock(mutex);++wifiSequence;if(!storage){++wifiDropped;return;}
 auto event=input;event.bootId=currentBootId;event.sequence=wifiSequence;auto& s=*storage;
 s.data.wifiEvents[s.wifiHead]=event;s.wifiHead=(s.wifiHead+1)%32;if(s.data.wifiCount<32)++s.data.wifiCount;
}
uint32_t wifiEventSequence(){std::lock_guard<std::mutex> lock(mutex);return wifiSequence;}
void observeNetwork(const AudioDiagnostics::NetworkSample& n,uint32_t at){
 std::lock_guard<std::mutex> lock(mutex);
 restoreOnce();
 if(!attempted){attempted=true;if(auto* p=allocate(sizeof(Storage)))storage=new(p)Storage;}
 if(!storage)return;auto& s=*storage;auto& d=s.data;
 Sample row;row.bootId=currentBootId;row.atMs=at;row.radio=latestRadio;row.generation=n.generation;row.trackHash=n.trackHash;row.elapsedMs=n.elapsedMs;
 row.pcmReady=n.buffered;row.pcmCapacity=n.capacity;row.compressedReady=n.compressedReady;row.compressedCapacity=n.compressedCapacity;
 row.underruns=n.underruns;row.outputErrors=n.outputErrors;row.readCalls=n.readCalls;row.maxReadUs=n.readMaxUs;row.readBytes=n.readBytes;row.readUs=n.readUs;
 row.rssi=n.rssi;row.wifiSleepRequested=n.sleep;row.phase=n.phase;row.jobs=n.jobs;
 const bool same=s.seen&&s.previousGeneration==n.generation&&s.previousHash==n.trackHash;
 const bool underrun=n.underruns>(same?s.previousUnderruns:0);
 if(underrun){d.beforeCount=std::min<size_t>(8,d.count);const size_t first=(s.head+historyCapacity-d.beforeCount)%historyCapacity;for(size_t i=0;i<d.beforeCount;++i)d.beforeUnderrun[i]=d.rows[(first+i)%historyCapacity];d.underrun=row;d.hasUnderrun=true;}
 s.seen=true;s.previousGeneration=n.generation;s.previousHash=n.trackHash;s.previousUnderruns=n.underruns;
 if(d.count){const auto& previous=d.rows[(s.head+historyCapacity-1)%historyCapacity];
  if(!underrun&&same&&!changed(row.radio,previous.radio)&&uint32_t(at-previous.atMs)<1000)return;
 }
 d.rows[s.head]=row;s.head=(s.head+1)%historyCapacity;if(d.count<historyCapacity)++d.count;d.available=true;
}
Capture* capture(){
 auto* memory=allocate(sizeof(Capture));if(!memory)return nullptr;auto* copy=new(memory)Capture;
 {std::lock_guard<std::mutex> lock(mutex);restoreOnce();if(storage){const auto& s=*storage;copy->count=s.data.count;copy->available=true;copy->hasUnderrun=s.data.hasUnderrun;copy->underrun=s.data.underrun;copy->beforeCount=s.data.beforeCount;copy->restoredAfterRestart=s.data.restoredAfterRestart;
  copy->retainedCount=s.data.retainedCount;copy->retainedWifiCount=s.data.retainedWifiCount;copy->wifiCount=s.data.wifiCount;
  for(size_t i=0;i<copy->retainedCount;++i)copy->retainedHistory[i]=s.data.retainedHistory[i];
  for(size_t i=0;i<copy->retainedWifiCount;++i)copy->retainedWifiEvents[i]=s.data.retainedWifiEvents[i];
  const size_t firstEvent=(s.wifiHead+32-s.data.wifiCount)%32;for(size_t i=0;i<copy->wifiCount;++i)copy->wifiEvents[i]=s.data.wifiEvents[(firstEvent+i)%32];
  for(size_t i=0;i<copy->beforeCount;++i)copy->beforeUnderrun[i]=s.data.beforeUnderrun[i];
  const size_t first=(s.head+historyCapacity-s.data.count)%historyCapacity;
  for(size_t i=0;i<copy->count;++i)copy->rows[i]=s.data.rows[(first+i)%historyCapacity];}
  copy->bootId=currentBootId;copy->resetReason=currentResetReason;copy->wifiObserverRegistered=observerRegistered;copy->wifiEventsDropped=wifiDropped;copy->retentionRestoreFailed=retentionRestoreFailed;}
 return copy;
}
void release(Capture* value){if(value){value->~Capture();free(value);}}
bool formatHeader(char* out,size_t size,const Capture& c){
 if(!out||!size)return false;
 const int n=snprintf(out,size,
 "{\"schema\":2,\"available\":%s,\"currentBootId\":%u,\"currentBootResetReason\":%d,\"wifiObserverRegistered\":%s,\"wifiRecorderUnavailableDrops\":%u,\"retentionRestoreFailed\":%s,"
 "\"historyCapacity\":180,\"nominalPeriodMs\":1000,\"retention\":\"Current and retained histories/events are separate; every record has bootId; Network stop retains 8 pre-underrun, 8 latest and 8 Wi-Fi events; power loss clears\","
 "\"units\":{\"intervalUnits\":\"1.25 ms\",\"timeoutUnits\":\"10 ms\",\"latency\":\"connection events\",\"bufferedFrames\":\"stereo PCM frames\",\"compressedReady\":\"committed bytes\"},"
 "\"columns\":[\"uptimeMs\",\"generation\",\"trackHash\",\"elapsedMs\",\"bufferedFrames\",\"capacityFrames\",\"compressedReady\",\"compressedCapacity\",\"underruns\",\"outputErrors\",\"readCalls\",\"readBytes\",\"readUs\",\"maxReadUs\",\"rssiDbm\",\"wifiSleepRequested\",\"phase\",\"jobs\",\"radioSampleMs\",\"stackReady\",\"connected\",\"encrypted\",\"bonded\",\"connecting\",\"connectionEpoch\",\"handle\",\"queryResult\",\"intervalUnits\",\"latency\",\"timeoutUnits\",\"softwareScan\",\"actualScan\",\"scanPurpose\",\"stopRequests\",\"stopFailures\",\"scanEndReason\",\"disconnectReason\",\"reports\",\"reportOverflows\",\"queuedReports\",\"wifiSleepActual\",\"wifiSleepResult\",\"radioQueryUs\",\"bootId\",\"apQueryResult\",\"apBssid\",\"apChannel\",\"apRssiDbm\",\"wifiEventSequence\"]",
 c.available?"true":"false",unsigned(c.bootId),c.resetReason,c.wifiObserverRegistered?"true":"false",unsigned(c.wifiEventsDropped),c.retentionRestoreFailed?"true":"false");
 return n>=0&&size_t(n)<size;
}
bool formatRow(char* out,size_t size,const Sample& s){
 if(!out||!size)return false;const auto& r=s.radio;
 const int n=snprintf(out,size,"[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%llu,%llu,%u,%d,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%u,%u,%u,%d,%d,%u,%u,%d,\"%02x:%02x:%02x:%02x:%02x:%02x\",%u,%d,%u]",
 unsigned(s.atMs),unsigned(s.generation),unsigned(s.trackHash),unsigned(s.elapsedMs),unsigned(s.pcmReady),unsigned(s.pcmCapacity),unsigned(s.compressedReady),unsigned(s.compressedCapacity),unsigned(s.underruns),unsigned(s.outputErrors),unsigned(s.readCalls),(unsigned long long)s.readBytes,(unsigned long long)s.readUs,unsigned(s.maxReadUs),int(s.rssi),int(s.wifiSleepRequested),unsigned(s.phase),unsigned(s.jobs),unsigned(r.atMs),unsigned(r.stackReady),unsigned(r.connected),unsigned(r.encrypted),unsigned(r.bonded),unsigned(r.connecting),unsigned(r.connectionEpoch),unsigned(r.handle),int(r.queryResult),unsigned(r.intervalUnits),unsigned(r.latency),unsigned(r.timeoutUnits),unsigned(r.softwareScan),unsigned(r.actualScan),unsigned(r.scanPurpose),unsigned(r.stopRequests),unsigned(r.stopFailures),int(r.scanEndReason),int(r.disconnectReason),unsigned(r.reports),unsigned(r.overflows),unsigned(r.queuedReports),int(r.wifiSleepActual),int(r.wifiSleepResult),unsigned(r.queryUs),unsigned(s.bootId),int(r.apQueryResult),unsigned(r.apBssid[0]),unsigned(r.apBssid[1]),unsigned(r.apBssid[2]),unsigned(r.apBssid[3]),unsigned(r.apBssid[4]),unsigned(r.apBssid[5]),unsigned(r.apChannel),int(r.apRssi),unsigned(r.wifiSequence));
 return n>=0&&size_t(n)<size;
}
bool formatWifiEvent(char* out,size_t size,const WifiEvent& e){
 if(!out||!size)return false;const char* kind="unknown";
 switch(e.kind){case WifiKind::Started:kind="started";break;case WifiKind::Connected:kind="connected";break;case WifiKind::GotIp:kind="got_ip";break;case WifiKind::Disconnected:kind="disconnected";break;case WifiKind::Stopped:kind="stopped";break;}
 const int n=snprintf(out,size,"{\"bootId\":%u,\"uptimeMs\":%u,\"sequence\":%u,\"kind\":\"%s\",\"bssid\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"channel\":%u,\"reason\":%u,\"rssiDbm\":%d,\"ipv4NetworkOrder\":%u}",unsigned(e.bootId),unsigned(e.atMs),unsigned(e.sequence),kind,unsigned(e.bssid[0]),unsigned(e.bssid[1]),unsigned(e.bssid[2]),unsigned(e.bssid[3]),unsigned(e.bssid[4]),unsigned(e.bssid[5]),unsigned(e.channel),unsigned(e.reason),int(e.rssi),unsigned(e.ip));
 return n>=0&&size_t(n)<size;
}
}
