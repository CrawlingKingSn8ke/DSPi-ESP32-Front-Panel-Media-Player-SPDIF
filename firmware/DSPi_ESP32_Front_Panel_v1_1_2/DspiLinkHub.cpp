#include "DspiLinkHub.h"
#include "DspiLinkNetwork.h"
#include "DspiLinkHubBoundary.h"
#include "DspiLinkAssets.h"
#include "DspiLinkSpectrum.h"
#include "DspiLinkControl.h"
#include "PanelWifi.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
namespace DspiLink {
namespace {
struct HubState {
  hub_t hub{};LinkNetworkSettings settings{};HubStatus status{};
  SocketToken sessions[2]{};bool localJob=false,mdns=false,devicePresent=false;
  uint32_t retryAt=0;char serial[17]{};
  uint32_t generation=0;bool mutation=false,controlChanged=false;SpectrumDemand analyser;
  struct Meta {uint32_t tag=0,generations[2]{};uint8_t request=0,sessions=0;uint16_t value=0;bool get=false,writes=false;} meta[Broker::Capacity];
};
HubState* state=nullptr;
bool sendText(void*,int slot,const char* text,size_t size){
  if(slot<0||slot>=2||!state)return false;
  cJSON* json=cJSON_ParseWithLength(text,size);
  if(!json)return false;panel_hub_filter_capabilities(json);
  char* filtered=cJSON_PrintUnformatted(json);cJSON_Delete(json);
  if(!filtered)return false;
  bool ok=sendNetwork(state->sessions[slot],1,(const uint8_t*)filtered,strlen(filtered));cJSON_free(filtered);return ok;
}
bool sendBinary(void*,int slot,const uint8_t* data,size_t size){return state&&slot>=0&&slot<2&&sendNetwork(state->sessions[slot],2,data,size);}
void ping(void*,int slot){if(state&&slot>=0&&slot<2)sendNetwork(state->sessions[slot],9,nullptr,0);}
void close(void*,int slot,uint16_t code){if(state&&slot>=0&&slot<2)closeNetwork(state->sessions[slot],code);}
uint32_t now(void*){return millis();}
bool submitHub(void*,const dspi_request_t* request,bool replay,uint32_t tag){
  if(!state||state->status.stopping||state->localJob)return false;
  const auto& pending=state->hub.pending[(state->hub.pending_head+state->hub.pending_count)%HUB_PENDING_CAPACITY];
  TransportRequest input{};input.owner=LinkOwner::Network;input.request=*request;input.mayRetry=replay;input.job=tag;
  if(pending.kind==HUB_WORK_CLIENT_COMMAND&&pending.slot>=0&&pending.slot<2){input.session=pending.slot;input.sessionGeneration=state->sessions[pending.slot].generation;}
  else input.session=0xffff;
  HubState::Meta* meta=nullptr;for(auto& m:state->meta)if(!m.tag){meta=&m;break;}
  if(!meta)return false;
  LinkTicket ticket;if(submit(input,ticket)!=LinkAdmission::Accepted)return false;
  meta->tag=tag;meta->request=request->request;meta->value=request->value;meta->get=request->is_get;meta->writes=pending.writes;
  if(input.session<2)meta->sessions=uint8_t(1u<<input.session);
  else if(pending.kind==HUB_WORK_POLL){
    for(unsigned i=0;i<2;++i)if(state->hub.sessions[i].state==HUB_SESSION_OPEN){
      for(const auto& p:state->hub.sessions[i].polls)if(p.active&&p.key.request==request->request&&p.key.value==request->value&&p.key.index==request->index&&p.key.length==request->length)meta->sessions|=uint8_t(1u<<i);
    }
  }
  for(unsigned i=0;i<2;++i)meta->generations[i]=state->sessions[i].generation;
  return true;
}
void advertise(void*){
  if(!state)return;
  char* text=hub_info_json(&state->hub);if(text){setNetworkInfo(text);cJSON_free(text);}
  if(!state->mdns)return;
  hub_advert_t a{};hub_advert(&state->hub,&a);
  // Link spec 3.1: the instance is named after the hub and follows renames.
  if(a.name&&a.name[0])mdns_service_instance_name_set("_dspi","_tcp",a.name);
  MDNS.addServiceTxt("dspi","tcp","v","1");
  MDNS.addServiceTxt("dspi","tcp","hid",a.hub_id);
  MDNS.addServiceTxt("dspi","tcp","kind","bridge");
  MDNS.addServiceTxt("dspi","tcp","auth",a.auth);
  MDNS.addServiceTxt("dspi","tcp","n",String(a.devices));
  MDNS.addServiceTxt("dspi","tcp","web","1");
  if(a.serial)MDNS.addServiceTxt("dspi","tcp","d",a.serial);
  else mdns_service_txt_item_remove("_dspi","_tcp","d");
}
bool apply(void*,const bridge_settings_t* settings){
  if(!state||settings->uart_tx_pin!=17||settings->uart_rx_pin!=16||settings->led_type!=SETTINGS_LED_NONE||settings->led_pin!=BOARD_PIN_NONE)return false;
  // Name changes are live; a hostname change is deliberately refused while
  // online, so the server's Origin policy cannot lag behind its public name.
  if(strcmp(settings->hostname,state->settings.bridge.hostname))return false;
  if(!saveLinkBridge(*settings))return false;state->settings.bridge=*settings;return true;
}
hub_io_result_t scan(void*,int,uint16_t,int){return HUB_IO_FAILED;}
bool join(void*,const char*,const char*){return false;}
void restart(void*,hub_restart_level_t){/* Denied by panel_hub_text before dispatch. */}
bool saveName(void*,const char* serial,const char* name){return saveLinkDeviceName(serial,name);}
bool loadName(void*,const char* serial,char* name,size_t size){return loadLinkDeviceName(serial,name,size);}
NetworkAsset assets(const char* path){
  const auto* a=panel_link_asset(path);
  return a?NetworkAsset{a->data,a->length,a->content_type,true}:NetworkAsset{};
}
void describe(void*,cJSON* object){
  cJSON_AddNumberToObject(object,"heap_free",ESP.getFreeHeap());cJSON_AddNumberToObject(object,"psram_free",ESP.getFreePsram());
  cJSON_AddStringToObject(object,"firmware","DSPi ESP32 Front Panel Link Test");
}
}
bool linkPinSet(){hub_auth_store_t saved{};bool bad=false;return loadLinkAuth(saved,bad)&&saved.pin_enabled&&!bad;}
bool setLinkPin(const char* pin){
  if(!hub_auth_pin_is_valid(pin))return false;
  if(state)return hub_set_pin(&state->hub,pin)==HUB_AUTH_OK;
  hub_auth_t auth{};auto crypto=linkCrypto();hub_auth_init(&auth,&crypto,nullptr);
  return hub_auth_set_pin(&auth,pin)==HUB_AUTH_OK&&saveLinkAuth(auth.store);
}
bool revokeLinkClient(uint16_t cid){
  if(state){
    if(!hub_auth_revoke(&state->hub.auth,cid))return false;
    for(unsigned i=0;i<2;++i)if(state->hub.sessions[i].cid==cid)hub_close_session(&state->hub,i,4001);
    return true;
  }
  hub_auth_store_t saved{};bool bad=false;if(!loadLinkAuth(saved,bad)||bad)return false;
  auto crypto=linkCrypto();hub_auth_t auth{};hub_auth_init(&auth,&crypto,&saved);
  return hub_auth_revoke(&auth,cid)&&saveLinkAuth(auth.store);
}
bool startHub(const LinkNetworkSettings& settings){
  if(state||!transportRunning()||!settings.ssid[0])return false;
  hub_auth_store_t saved{};bool bad=false;if(!loadLinkAuth(saved,bad)||bad||!saved.pin_enabled)return false;
  state=new(std::nothrow) HubState();if(!state)return false;
  state->settings=settings;state->status.active=true;state->status.pinSet=true;
  snprintf(state->status.ssid,sizeof(state->status.ssid),"%s",settings.ssid);snprintf(state->status.hostname,sizeof(state->status.hostname),"%s",settings.bridge.hostname);
  hub_io_t io{};io.send_text=sendText;io.send_binary=sendBinary;io.close=close;io.ping=ping;io.submit=submitHub;io.now_ms=now;
  io.apply_settings=apply;io.start_wifi_scan=scan;io.wifi_join=join;io.restart=restart;io.save_device_name=saveName;io.load_device_name=loadName;io.advertise=advertise;io.describe_bridge=describe;
  hub_config_t config{};config.settings=settings.bridge;config.board=&panelLinkBoard();config.max_sessions=2;config.version="1.5.1";config.reserved_poll_bps=3000;
  auto crypto=linkCrypto();hub_init(&state->hub,&io,&config,&crypto,&saved);
  if(!PanelWifi::beginStation(PanelWifi::Owner::Link,settings.ssid,settings.password,settings.bridge.hostname)){delete state;state=nullptr;return false;}
  state->retryAt=millis()+30000;return true;
}
void hubNativeNotification(const uint8_t* bytes,uint16_t size){if(state)hub_notification(&state->hub,bytes,size);}
void hubNativeReply(uint32_t tag,const dspi_reply_t& reply){
  if(!state)return;
  for(auto& m:state->meta)if(m.tag==tag){
    if(m.writes&&(reply.kind!=DSPI_REPLY_ANSWERED||reply.status==DSPI_STATUS_OK)){
      if(controlWrite(m.request))state->controlChanged=true;else state->mutation=true;
    }
    if(reply.kind==DSPI_REPLY_ANSWERED&&reply.status==DSPI_STATUS_OK){
      for(unsigned i=0;i<2;++i)if((m.sessions&(1u<<i))&&m.generations[i]==state->sessions[i].generation){
        if(m.get&&(m.request==0x0b||m.request==0x0c||m.request==0x0f))state->analyser.note(i,millis());
        if(!m.get&&m.request==0x08)state->analyser.note(i,millis());
        if(m.get&&m.request==0x0e&&m.value==0)state->analyser.closed(i);
      }
    }
    m={};break;
  }
  hub_reply(&state->hub,tag,&reply);
}
bool takeHubMutation(){if(!state)return false;bool changed=state->mutation;state->mutation=false;return changed;}
bool takeHubControlChange(){if(!state)return false;bool changed=state->controlChanged;state->controlChanged=false;return changed;}
bool hubExternalWritePossible(){
  if(!state)return false;
  if(state->mutation||state->hub.origin_count)return true;
  for(const auto& m:state->meta)if(m.tag&&m.writes)return true;
  return false;
}
bool remoteAnalyserActive(uint32_t nowMs){return state&&state->analyser.active(nowMs);}
bool hubAllowsLocal(const dspi_request_t& request){
  return !state||!state->hub.device.locked_by||Broker::safeReplay(request.is_get,request.request);
}
void serviceHub(bool localJob){
  if(!state)return;
  PanelWifi::service(millis());
  state->localJob=localJob;
  if(state->status.stopping){
    stopNetwork();if(!networkStopped())return;
    if(transportStatus().busy)return;
    if(state->mdns){MDNS.end();state->mdns=false;}
    if(!PanelWifi::release(PanelWifi::Owner::Link))return;
    delete state;state=nullptr;return;
  }
  const auto wire=transportStatus();
  if(wire.present&&(!state->devicePresent||wire.generation!=state->generation)){
    hub_device_found(&state->hub,&wire.identity,wire.baud);state->devicePresent=true;state->generation=wire.generation;snprintf(state->serial,sizeof(state->serial),"%s",wire.identity.serial);
  }else if(!wire.present&&state->devicePresent){hub_device_lost(&state->hub);state->devicePresent=false;}
  state->status.dspi=wire.present;
  if(WiFi.status()==WL_CONNECTED&&!state->status.online){
    auto ip=WiFi.localIP().toString();
    if(!startNetwork(state->settings.bridge.hostname,ip.c_str(),assets)){
      snprintf(state->status.error,sizeof(state->status.error),"Link server unavailable");return;
    }
    snprintf(state->status.ip,sizeof(state->status.ip),"%s",ip.c_str());state->status.online=true;state->status.error[0]=0;
    state->mdns=MDNS.begin(state->settings.bridge.hostname);
    if(state->mdns){MDNS.setInstanceName(state->settings.bridge.name);MDNS.addService("dspi","tcp",11915);}advertise(nullptr);
  }
  if(WiFi.status()!=WL_CONNECTED){
    if(state->status.online){stopNetwork();if(!networkStopped())return;state->status.online=false;if(state->mdns){MDNS.end();state->mdns=false;}
      for(int i=0;i<2;++i){hub_session_closed(&state->hub,i);state->analyser.closed(i);networkSession(i,0);state->sessions[i]={};}}
    if(int32_t(millis()-state->retryAt)>=0){WiFi.begin(state->settings.ssid,state->settings.password);state->retryAt=millis()+30000;}
  }
  NetworkEvent event{};
  for(unsigned i=0;i<12&&nextNetworkEvent(event);++i){
    const int slot=event.token.slot;
    if(slot>=0&&slot<2){
      if(event.kind==NetworkEventKind::Open){state->sessions[slot]=event.token;networkSession(slot,event.token.generation);hub_session_opened(&state->hub,slot,event.address);}
      else if(state->sessions[slot].generation==event.token.generation){
        if(event.kind==NetworkEventKind::Text)panel_hub_text(&state->hub,slot,(const char*)event.data,event.length,localJob);
        if(event.kind==NetworkEventKind::Binary)hub_session_binary(&state->hub,slot,event.data,event.length);
        if(event.kind==NetworkEventKind::Activity)hub_session_activity(&state->hub,slot);
        if(event.kind==NetworkEventKind::Closed){hub_session_closed(&state->hub,slot);state->analyser.closed(slot);networkSession(slot,0);releaseNetworkSession(event.token);state->sessions[slot]={};}
      }else if(event.kind==NetworkEventKind::Closed)releaseNetworkSession(event.token);
    }
    finishNetworkEvent(event);
  }
  hub_tick(&state->hub);state->status.clients=0;
  for(unsigned i=0;i<2;++i)if(state->hub.sessions[i].state==HUB_SESSION_OPEN)++state->status.clients;
}
void stopHub(){if(state){state->status.stopping=true;for(int i=0;i<2;++i)networkSession(i,0);}}
bool hubStopped(){return !state;}
HubStatus hubStatus(){return state?state->status:HubStatus{};}
}
