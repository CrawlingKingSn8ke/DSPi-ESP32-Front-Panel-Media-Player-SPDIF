#pragma once
#include "DspiLinkSetup.h"
#include "PanelWifiSetupPolicy.h"
extern "C" {
#include "src/dspi_link_vendor/hub_auth.h"
#include "cJSON.h"
}
namespace DspiLink {
struct SetupCredentials {char ssid[33]{},password[64]{};};
inline unsigned setupTokenCredentials(const PanelWifi::SetupPolicy& session,const uint8_t* bytes,size_t size,uint32_t now,SetupCredentials& out){
  out={};
  if(!bytes||!size||size>SetupBodyLimit||memchr(bytes,0,size)||!validUtf8(bytes,size))return 400;
  char text[SetupBodyLimit+1];memcpy(text,bytes,size);text[size]=0;
  if(!jsonDepthAllowed(text)||strstr(text,"\\u0000"))return 400;
  cJSON* json=cJSON_ParseWithLengthOpts(text,size+1,nullptr,true);
  unsigned result=400;
  if(json&&cJSON_IsObject(json)&&cJSON_GetArraySize(json)==3){
    const cJSON *token=nullptr,*ssid=nullptr,*password=nullptr;bool unique=true;
    for(auto* item=json->child;item;item=item->next){
      if(!item->string){unique=false;break;}
      if(!strcmp(item->string,"token")){if(token)unique=false;token=item;}
      else if(!strcmp(item->string,"ssid")){if(ssid)unique=false;ssid=item;}
      else if(!strcmp(item->string,"password")){if(password)unique=false;password=item;}
      else unique=false;
    }
    if(unique&&cJSON_IsString(token)&&cJSON_IsString(ssid)&&cJSON_IsString(password)){
      PanelWifi::Credentials candidate{};
      if(strlen(ssid->valuestring)<sizeof candidate.ssid&&strlen(password->valuestring)<sizeof candidate.password){
        strcpy(candidate.ssid,ssid->valuestring);strcpy(candidate.password,password->valuestring);
        if(PanelWifi::validCredentials(candidate)){
          result=session.authorized(token->valuestring,now)?200:403;
          if(result==200){strcpy(out.ssid,candidate.ssid);strcpy(out.password,candidate.password);}
        }
      }
      volatile char* clear=reinterpret_cast<volatile char*>(&candidate);
      for(size_t n=0;n<sizeof candidate;++n)clear[n]=0;
    }
  }
  if(json){for(auto* item=json->child;item;item=item->next)if(cJSON_IsString(item)&&item->valuestring){const size_t length=strlen(item->valuestring);volatile char* clear=item->valuestring;for(size_t n=0;n<length;++n)clear[n]=0;}cJSON_Delete(json);}
  volatile char* clear=text;for(size_t n=0;n<sizeof text;++n)clear[n]=0;
  return result;
}
inline unsigned setupCredentials(hub_auth_t& auth,uint32_t address,const uint8_t* bytes,size_t size,SetupCredentials& out){
  out={};
  if(!bytes||!size||size>SetupBodyLimit||memchr(bytes,0,size)||!validUtf8(bytes,size))return 400;
  char text[SetupBodyLimit+1];memcpy(text,bytes,size);text[size]=0;
  if(!jsonDepthAllowed(text)||strstr(text,"\\u0000"))return 400;
  cJSON* json=cJSON_ParseWithLengthOpts(text,size+1,nullptr,true);
  if(!json)return 400;
  const auto* pin=cJSON_GetObjectItemCaseSensitive(json,"pin");
  const auto* ssid=cJSON_GetObjectItemCaseSensitive(json,"ssid");
  const auto* password=cJSON_GetObjectItemCaseSensitive(json,"password");
  if(!cJSON_IsObject(json)||cJSON_GetArraySize(json)!=3||!cJSON_IsString(pin)||!cJSON_IsString(ssid)||!cJSON_IsString(password)||
     !hub_auth_pin_is_valid(pin->valuestring)||!strlen(ssid->valuestring)||strlen(ssid->valuestring)>32||strlen(password->valuestring)>63||
     (strlen(password->valuestring)&&strlen(password->valuestring)<8)) {cJSON_Delete(json);return 400;}
  // Exactly three unique keys; duplicate/unknown keys cannot shadow values.
  unsigned p=0,s=0,w=0;
  for(auto* item=json->child;item;item=item->next){
    if(!strcmp(item->string,"pin"))++p;else if(!strcmp(item->string,"ssid"))++s;else if(!strcmp(item->string,"password"))++w;
  }
  if(p!=1||s!=1||w!=1){cJSON_Delete(json);return 400;}
  const auto result=hub_auth_check_pin(&auth,address,pin->valuestring);
  if(result==HUB_AUTH_OK){strcpy(out.ssid,ssid->valuestring);strcpy(out.password,password->valuestring);}
  cJSON_Delete(json);memset(text,0,sizeof(text));
  return result==HUB_AUTH_OK?200:result==HUB_AUTH_RATE_LIMITED?429:result==HUB_AUTH_UNAVAILABLE?503:403;
}
}
