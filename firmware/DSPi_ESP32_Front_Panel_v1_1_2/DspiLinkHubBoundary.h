#pragma once
#include <string.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#include "src/dspi_link_vendor/hub.h"
#include "src/dspi_link_vendor/hub_internal.h"

// This adapter owns the combined-panel restrictions; the upstream hub remains
// wire compatible. The JSON has already passed the portable depth/UTF-8 gate.
static inline void panel_hub_text(hub_t* hub,int slot,const char* text,size_t size,bool local_job){
  if(slot<0||(size_t)slot>=hub->config.max_sessions)return;
  if(hub->sessions[slot].state!=HUB_SESSION_OPEN){hub_session_text(hub,slot,text,size);return;}
  cJSON* message=cJSON_ParseWithLength(text,size);
  if(!message){hub_session_text(hub,slot,text,size);return;}
  const cJSON* type=cJSON_GetObjectItemCaseSensitive(message,"t");
  const cJSON* id=cJSON_GetObjectItemCaseSensitive(message,"id");
  const char* t=cJSON_IsString(type)?type->valuestring:"";
  const int request_id=cJSON_IsNumber(id)?id->valueint:-1;
  const char* denied=NULL;
  if(!strcmp(t,"hub.reset")||!strcmp(t,"hub.restart")||!strncmp(t,"hub.update",10))denied="use panel Transfer/Update or local Link settings";
  if(!strncmp(t,"hub.wifi.",9))denied="use Network Setup on the front panel";
  if(!strcmp(t,"hub.auth.set")){
    const cJSON* mode=cJSON_GetObjectItemCaseSensitive(message,"mode");
    if(!cJSON_IsString(mode)||strcmp(mode->valuestring,"pin"))denied="this panel requires a PIN";
  }
  if(!strcmp(t,"hub.config.set")){
    const cJSON* config=cJSON_GetObjectItemCaseSensitive(message,"config");
    if(cJSON_GetObjectItemCaseSensitive(config,"uart")||cJSON_GetObjectItemCaseSensitive(config,"led"))denied="panel UART and LED assignments are fixed";
  }
  const bool busy=local_job&&!strcmp(t,"lock.acquire");
  cJSON_Delete(message);
  if(denied)hub_reply_error(hub,slot,request_id,"unsupported",denied);
  else if(busy)hub_reply_error(hub,slot,request_id,"busy","panel operation in progress");
  else hub_session_text(hub,slot,text,size);
}
static inline void panel_hub_filter_capabilities(cJSON* object){
  cJSON* caps=cJSON_GetObjectItemCaseSensitive(object,"caps");
  for(int i=cJSON_GetArraySize(caps)-1;i>=0;--i){
    const cJSON* c=cJSON_GetArrayItem(caps,i);
    if(cJSON_IsString(c)&&(!strcmp(c->valuestring,"reset")||!strcmp(c->valuestring,"hub_update")||!strcmp(c->valuestring,"wifi")))cJSON_DeleteItemFromArray(caps,i);
  }
}
#ifdef __cplusplus
}
#endif
