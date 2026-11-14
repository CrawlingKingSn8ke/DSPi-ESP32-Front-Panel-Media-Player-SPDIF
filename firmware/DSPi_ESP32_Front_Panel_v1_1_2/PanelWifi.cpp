#include "PanelWifi.h"
#include <Arduino.h>
#include <WiFi.h>
#include <string.h>

namespace PanelWifi {
namespace {
Policy policy;
bool networkSleepSaved=false;
bool networkJoinPolicySet=false;
wifi_ps_type_t previousNetworkSleep=WIFI_PS_MIN_MODEM;
}
bool acquire(Owner owner){return policy.acquire(owner);}
bool owns(Owner owner){return policy.owns(owner);}
bool release(Owner owner){
  if(!policy.owns(owner)&&!policy.stopping(owner))return true;
  // Call only after the owning service has drained its sockets/workers.
  policy.release(owner);WiFi.setAutoReconnect(false);
  if(!WiFi.mode(WIFI_OFF))return false;
  if(owner==Owner::Network&&networkJoinPolicySet){
    // These setters cache values even with the radio off. Restore the installed
    // Arduino core's defaults for Link/Transfer/Setup; it exposes no getters.
    WiFi.setScanMethod(WIFI_FAST_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    networkJoinPolicySet=false;
  }
  if(owner==Owner::Network&&networkSleepSaved){
    if(!WiFi.setSleep(previousNetworkSleep))return false;
    networkSleepSaved=false;
  }
  policy.stopped();return true;
}
bool beginStation(Owner owner,const char* ssid,const char* password,const char* hostname){
  if(owner==Owner::Setup||!ssid||!ssid[0]||!password||!hostname||!policy.acquire(owner))return false;
  // Arduino defaults to modem sleep. Synchronous SMB reads must not wait for
  // router DTIM wakeups. Cache NONE before STA_START applies the sleep mode;
  // retain the original once, including across repeated joins/reconnects.
  if(owner==Owner::Network&&!networkSleepSaved){
    const auto previous=WiFi.getSleep();
    if(!WiFi.setSleep(WIFI_PS_NONE)){release(owner);return false;}
    previousNetworkSleep=previous;networkSleepSaved=true;
  }
  if(owner==Owner::Network){
    // FAST_SCAN stops at the first SSID match, even when a nearer mesh node
    // exists. Let the driver collect all matches and try strongest first,
    // with its normal fallback and no pinned BSSID or playback-time scans.
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    networkJoinPolicySet=true;
  }
  if(!WiFi.mode(WIFI_STA)){release(owner);return false;}
  WiFi.setHostname(hostname);WiFi.setAutoReconnect(true);WiFi.begin(ssid,password);return true;
}
bool beginCandidate(const char* ssid,const char* password){
  if(!owns(Owner::Setup)||!ssid||!ssid[0]||!password)return false;
  WiFi.setAutoReconnect(false);WiFi.disconnect(false,false);WiFi.begin(ssid,password);return true;
}
void cancelCandidate(){if(owns(Owner::Setup))WiFi.disconnect(false,false);}
bool connectedTo(const char* ssid){return ssid&&WiFi.status()==WL_CONNECTED&&WiFi.SSID()==ssid;}
void service(uint32_t){
  if(policy.state()==State::Off||policy.state()==State::Stopping||owns(Owner::Setup))return;
  if(WiFi.status()==WL_CONNECTED)policy.connected();else policy.disconnected();
}
int signalStrength(){return WiFi.status()==WL_CONNECTED?WiFi.RSSI():-127;}
int powerSaveMode(){return int(WiFi.getSleep());}
Snapshot snapshot(){
  Snapshot out;out.state=policy.state();out.stationConnected=WiFi.status()==WL_CONNECTED;
  if(out.stationConnected){auto ssid=WiFi.SSID();auto ip=WiFi.localIP().toString();snprintf(out.ssid,sizeof out.ssid,"%s",ssid.c_str());snprintf(out.ip,sizeof out.ip,"%s",ip.c_str());}
  return out;
}
}
