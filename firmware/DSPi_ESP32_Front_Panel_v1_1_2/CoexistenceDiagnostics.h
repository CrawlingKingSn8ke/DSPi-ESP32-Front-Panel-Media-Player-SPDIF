#pragma once
#ifndef DSPI_COEXISTENCE_DIAGNOSTICS_H
#define DSPI_COEXISTENCE_DIAGNOSTICS_H
#include "AudioDiagnostics.h"
#include <cstddef>
#include <cstdint>
namespace CoexistenceDiagnostics {
// Observation only. AP BSSID is recorded; no credentials/controller policy.
struct Radio {
 uint32_t atMs=0,connectionEpoch=0,reports=0,overflows=0,queuedReports=0;
 uint32_t stopRequests=0,stopFailures=0,queryUs=0;
 int32_t disconnectReason=0,scanEndReason=-1,queryResult=-1,wifiSleepActual=-1,wifiSleepResult=-1;
 uint16_t handle=0xffff,intervalUnits=0,latency=0,timeoutUnits=0;
 uint8_t scanPurpose=0;
 bool stackReady=false,connected=false,encrypted=false,bonded=false;
 bool softwareScan=false,actualScan=false,connecting=false;
 int32_t apQueryResult=-1;uint32_t wifiSequence=0;
 uint8_t apBssid[6]={0},apChannel=0;int8_t apRssi=-127;
};
struct Sample {
 uint32_t atMs=0,generation=0,trackHash=0,elapsedMs=0,bootId=0;
 uint32_t pcmReady=0,pcmCapacity=0,compressedReady=0,compressedCapacity=0;
 uint32_t underruns=0,outputErrors=0,readCalls=0,maxReadUs=0;
 uint64_t readBytes=0,readUs=0;
 int32_t rssi=-127,wifiSleepRequested=-1;uint8_t phase=0,jobs=0;
 Radio radio;
};
constexpr size_t historyCapacity=180;
enum class WifiKind:uint8_t {Started=1,Connected,GotIp,Disconnected,Stopped};
struct WifiEvent {uint32_t bootId=0,atMs=0,sequence=0,ip=0;uint16_t reason=0;uint8_t bssid[6]={0};WifiKind kind=WifiKind::Started;uint8_t channel=0;int8_t rssi=-127;};
struct Capture {
 Sample rows[historyCapacity],underrun,beforeUnderrun[8],retainedHistory[8];
 WifiEvent wifiEvents[32],retainedWifiEvents[8];
 size_t count=0,beforeCount=0,retainedCount=0,wifiCount=0,retainedWifiCount=0;
 uint32_t bootId=1,wifiEventsDropped=0;int resetReason=0;
 bool hasUnderrun=false,available=false,restoredAfterRestart=false,wifiObserverRegistered=false,retentionRestoreFailed=false;
};
struct Retained {uint32_t magic=0,checksum=0,count=0,beforeCount=0,wifiCount=0;bool hasUnderrun=false;Sample latest[8],beforeUnderrun[8],underrun;WifiEvent wifiEvents[8];};
static_assert(sizeof(Retained)<=4096,"Retained diagnostic summary must fit bounded RTC budget");
void saveRetained(Retained&,const Capture&);
bool restoreRetained(Retained&,bool softwareReset,Capture&);
void preserveForRestart();
void beginBoot(uint32_t id,int resetReason);
void wifiObserverRegistration(bool okay);
void observeWifiEvent(const WifiEvent&);
uint32_t wifiEventSequence();
void observeRadio(const Radio&);
void observeNetwork(const AudioDiagnostics::NetworkSample&,uint32_t uptimeMs);
// Independent PSRAM copy: no recorder lock is held during HTTP transmission.
Capture* capture();
void release(Capture*);
// Header intentionally leaves the JSON object open for streamed rows.
bool formatHeader(char*,size_t,const Capture&);
bool formatRow(char*,size_t,const Sample&);
bool formatWifiEvent(char*,size_t,const WifiEvent&);
}
#endif
