#include <cassert>
#include <cstdio>
#include <WiFi.h>
#include "../../firmware/DSPi_ESP32_Front_Panel_v1_1_2/PanelWifi.h"
int main(){
 using namespace PanelWifi;
 // MIN is the installed Arduino core's cold-boot default. NONE must already
 // be cached for STA_START, not only selected after begin() has started.
 for(auto original:{WIFI_PS_MIN_MODEM,WIFI_PS_MAX_MODEM,WIFI_PS_NONE}){
  WiFi=HostWiFi{};WiFi.sleepMode=original;
  assert(beginStation(Owner::Network,"Home","secret","panel"));
  assert(WiFi.joinedScan==WIFI_ALL_CHANNEL_SCAN&&WiFi.joinedSort==WIFI_CONNECT_AP_BY_SIGNAL);
  assert(WiFi.startedSleep==WIFI_PS_NONE&&WiFi.joinedSleep==WIFI_PS_NONE);
  assert(owns(Owner::Network));auto count=WiFi.sleepCalls;
  assert(!beginStation(Owner::Link,"Other","secret","other")&&WiFi.sleepCalls==count);
  assert(release(Owner::Link)&&WiFi.sleepMode==WIFI_PS_NONE&&WiFi.radioMode==WIFI_STA);
  assert(beginStation(Owner::Network,"Home","secret","panel"));
  assert(WiFi.joinedScan==WIFI_ALL_CHANNEL_SCAN&&WiFi.scanCalls==0);
  WiFi.connected=false;service(10);assert(snapshot().state==State::Retrying&&WiFi.sleepMode==WIFI_PS_NONE);
  WiFi.connected=true;service(20);assert(snapshot().state==State::Online&&WiFi.sleepMode==WIFI_PS_NONE);
  auto joins=WiFi.joinCalls;
  for(unsigned i=0;i<1000;++i)service(30+i);
  assert(WiFi.joinCalls==joins&&WiFi.scanCalls==0);
  WiFi.modeSuccess=false;assert(!release(Owner::Network));assert(WiFi.sleepMode==WIFI_PS_NONE&&snapshot().state==State::Stopping);
  assert(!acquire(Owner::Link));WiFi.modeSuccess=true;assert(release(Owner::Network));
  assert(WiFi.sleepMode==original&&WiFi.radioMode==WIFI_OFF&&snapshot().state==State::Off);
  assert(WiFi.scanMethod==WIFI_FAST_SCAN&&WiFi.sortMethod==WIFI_CONNECT_AP_BY_SIGNAL);
 }
 // These paths must not acquire or modify the Network-only override.
 for(auto owner:{Owner::Link,Owner::Transfer}){
  WiFi=HostWiFi{};WiFi.sleepMode=WIFI_PS_MAX_MODEM;
  assert(beginStation(owner,"Home","secret","panel"));assert(WiFi.joinedSleep==WIFI_PS_MAX_MODEM&&!WiFi.sleepCalls);
  assert(WiFi.joinedScan==WIFI_FAST_SCAN&&WiFi.joinedSort==WIFI_CONNECT_AP_BY_SIGNAL);
  assert(release(owner)&&WiFi.sleepMode==WIFI_PS_MAX_MODEM&&!WiFi.sleepCalls);
 }
 WiFi=HostWiFi{};assert(acquire(Owner::Setup));assert(beginCandidate("Home","secret"));assert(!WiFi.sleepCalls);assert(WiFi.joinedScan==WIFI_FAST_SCAN);assert(release(Owner::Setup));
 // Failed override must not connect, steal ownership or leave the radio on.
 WiFi=HostWiFi{};WiFi.sleepFailValue=WIFI_PS_NONE;
 assert(!beginStation(Owner::Network,"Home","secret","panel"));assert(!WiFi.joinCalls&&snapshot().state==State::Off&&WiFi.sleepMode==WIFI_PS_MIN_MODEM);
 WiFi.sleepFailValue=-1;assert(beginStation(Owner::Network,"Home","secret","panel"));assert(release(Owner::Network));
 // The same driver instance must return to the original join policy for Link
 // and setup, including after Network's failed STA startup/shutdown retry.
 assert(beginStation(Owner::Link,"Home","secret","panel"));assert(WiFi.joinedScan==WIFI_FAST_SCAN);assert(release(Owner::Link));
 // Driver owns candidate fallback. A failed association leaves its strongest-
 // first configuration intact while service only reports retrying, not scans.
 WiFi=HostWiFi{};WiFi.forceJoinFailure=true;WiFi.sortMethod=WIFI_CONNECT_AP_BY_SECURITY;
 assert(beginStation(Owner::Network,"Home","secret","panel"));service(1);
 assert(snapshot().state==State::Retrying&&WiFi.joinedScan==WIFI_ALL_CHANNEL_SCAN&&WiFi.joinedSort==WIFI_CONNECT_AP_BY_SIGNAL);
 assert(!WiFi.scanCalls&&WiFi.joinCalls==1);assert(release(Owner::Network));
 assert(acquire(Owner::Setup));assert(beginCandidate("Home","secret"));
 assert(WiFi.joinedScan==WIFI_FAST_SCAN&&WiFi.joinedSort==WIFI_CONNECT_AP_BY_SIGNAL);assert(release(Owner::Setup));
 // Failed mode startup retains the shutdown lease until a real OFF succeeds.
 WiFi=HostWiFi{};WiFi.modeSuccess=false;assert(!beginStation(Owner::Network,"Home","secret","panel"));assert(!WiFi.joinCalls&&!acquire(Owner::Transfer));
 WiFi.modeSuccess=true;assert(release(Owner::Network)&&WiFi.sleepMode==WIFI_PS_MIN_MODEM);
 assert(WiFi.scanMethod==WIFI_FAST_SCAN);
 // A restoration failure cannot release the lease to a different service.
 WiFi=HostWiFi{};assert(beginStation(Owner::Network,"Home","secret","panel"));WiFi.sleepFailValue=WIFI_PS_MIN_MODEM;
 assert(!release(Owner::Network)&&!acquire(Owner::Link));WiFi.sleepFailValue=-1;assert(release(Owner::Network)&&WiFi.sleepMode==WIFI_PS_MIN_MODEM);
 puts("Actual Network Wi-Fi: strongest-first all-channel join, no manual playback scan; sleep/restoration/rejoin/error barriers and unchanged other owners PASS");
}
