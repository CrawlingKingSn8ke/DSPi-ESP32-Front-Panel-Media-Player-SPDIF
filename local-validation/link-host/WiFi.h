#pragma once
#include <Arduino.h>
#include <assert.h>
#define WIFI_OFF 0
#define WIFI_STA 1
#define WIFI_AP 2
#define WIFI_AP_STA 3
#define WIFI_SCAN_RUNNING -1
#define WIFI_SCAN_FAILED -2
#define WIFI_AUTH_OPEN 0
enum arduino_event_id_t {ARDUINO_EVENT_WIFI_SCAN_DONE};
enum wifi_ps_type_t {WIFI_PS_NONE,WIFI_PS_MIN_MODEM,WIFI_PS_MAX_MODEM};
enum wifi_scan_method_t {WIFI_FAST_SCAN,WIFI_ALL_CHANNEL_SCAN};
enum wifi_sort_method_t {WIFI_CONNECT_AP_BY_SIGNAL,WIFI_CONNECT_AP_BY_SECURITY};
#define WL_CONNECTED 3
struct HostIp {String toString()const{return "127.0.0.1";}};
struct HostWiFi {
  struct Result {String ssid;int rssi;int encryption=1;};
  std::vector<Result> results;
  int scanState=WIFI_SCAN_FAILED,scanCalls=0,radioMode=0,cleans=0;
  void (*scanCallback)(arduino_event_id_t)=nullptr;
  unsigned onEvent(void(*cb)(arduino_event_id_t),arduino_event_id_t){scanCallback=cb;return 1;}
  void finishScan(int state){scanState=state;if(scanCallback)scanCallback(ARDUINO_EVENT_WIFI_SCAN_DONE);}
  void setScanTimeout(uint32_t){}
  bool connected=false;
  String currentSsid;
  bool modeSuccess=true;
  bool forceJoinFailure=false;
  bool apSuccess=true;
  wifi_ps_type_t sleepMode=WIFI_PS_MIN_MODEM,startedSleep=WIFI_PS_MIN_MODEM,joinedSleep=WIFI_PS_MIN_MODEM;
  int sleepCalls=0,sleepFailValue=-1,joinCalls=0;
  wifi_scan_method_t scanMethod=WIFI_FAST_SCAN,joinedScan=WIFI_FAST_SCAN;
  wifi_sort_method_t sortMethod=WIFI_CONNECT_AP_BY_SIGNAL,joinedSort=WIFI_CONNECT_AP_BY_SIGNAL;
  void setScanMethod(wifi_scan_method_t value){scanMethod=value;}
  void setSortMethod(wifi_sort_method_t value){sortMethod=value;}
  wifi_ps_type_t getSleep()const{return sleepMode;}
  bool setSleep(wifi_ps_type_t value){++sleepCalls;if(int(value)==sleepFailValue)return false;sleepMode=value;return true;}
  bool setSleep(bool enabled){return setSleep(enabled?WIFI_PS_MIN_MODEM:WIFI_PS_NONE);}
  bool mode(int m){if(!modeSuccess)return false;radioMode=m;if(m==WIFI_OFF)connected=false;if(m==WIFI_STA)startedSleep=sleepMode;return true;}
  int16_t scanNetworks(bool async,bool hidden,bool passive,uint32_t dwell){assert(async&&!hidden&&!passive&&dwell<=150);++scanCalls;scanState=WIFI_SCAN_RUNNING;return scanState;}
  int16_t scanComplete(){return scanState;}
  void scanDelete(){assert(scanState!=WIFI_SCAN_RUNNING);scanState=WIFI_SCAN_FAILED;++cleans;}
  String SSID(uint8_t i){return results[i].ssid;}
  String SSID()const{return currentSsid;}
  int32_t RSSI(uint8_t i){return results[i].rssi;}
  int32_t RSSI(){return -60;}
  int encryptionType(uint8_t i){return results[i].encryption;}
  void setHostname(const char*){}
  void setAutoReconnect(bool){}
  void begin(const char* ssid,const char*){++joinCalls;joinedSleep=sleepMode;joinedScan=scanMethod;joinedSort=sortMethod;currentSsid=ssid;connected=!forceJoinFailure;}
  void disconnect(bool,bool){connected=false;}
  int status()const{return connected?WL_CONNECTED:0;}
  HostIp localIP()const{return {};}
  bool softAP(const char*,const char*,int,bool,int){return apSuccess;}
  bool softAPdisconnect(bool){return true;}
  HostIp softAPIP()const{return {};}
};
inline HostWiFi WiFi;
