#include "SmbSetupModel.h"
#include <cJSON.h>
#include <cstring>
namespace SmbLibrary {
namespace {
bool fields(cJSON* j,const char* const* allowed,unsigned n){
 if(!cJSON_IsObject(j))return false;
 for(auto* a=j->child;a;a=a->next){bool found=false;for(unsigned i=0;i<n;++i)if(a->string&&!strcmp(a->string,allowed[i]))found=true;
  if(!found)return false;for(auto* b=a->next;b;b=b->next)if(b->string&&!strcmp(a->string,b->string))return false;}
 return true;
}
template<size_t N>bool value(cJSON* j,const char* key,char (&out)[N]){auto* v=cJSON_GetObjectItemCaseSensitive(j,key);if(!cJSON_IsString(v)||strlen(v->valuestring)>=N)return false;strcpy(out,v->valuestring);return true;}
}
bool parseSetupEdit(const char* text,const Settings& original,SetupEdit& out){
 if(!text||strlen(text)>2048||strstr(text,"\\u0000"))return false;
 auto* j=cJSON_Parse(text);if(!j)return false;bool ok=false;
 const char* names[]={"action","slot","label","server","share","root","user","password","domain","keepPassword","token"};
 auto* a=cJSON_GetObjectItemCaseSensitive(j,"action");auto* slot=cJSON_GetObjectItemCaseSensitive(j,"slot");
 if(fields(j,names,11)&&cJSON_IsString(a)&&cJSON_IsNumber(slot)&&slot->valuedouble==slot->valueint&&slot->valueint>=0&&slot->valueint<int(kShares)){
  out.settings=original;out.slot=slot->valueint;out.remove=!strcmp(a->valuestring,"remove");
  if(out.remove)ok=removeShare(out.settings,out.slot);
  else if(!strcmp(a->valuestring,"save")){
   Share share;auto* keep=cJSON_GetObjectItemCaseSensitive(j,"keepPassword");
   ok=value(j,"label",share.label)&&value(j,"root",share.root)&&value(j,"server",share.connection.server)&&value(j,"share",share.connection.share)&&value(j,"user",share.connection.user)&&value(j,"password",share.connection.password)&&value(j,"domain",share.connection.domain)&&cJSON_IsBool(keep)&&replaceShare(out.settings,out.slot,share,cJSON_IsTrue(keep));
   memset(&share,0,sizeof share);
  }
 }
 cJSON_Delete(j);return ok;
}
bool setupList(const Settings& settings,const char* state,char* output,size_t capacity){
 auto* j=cJSON_CreateObject();if(!j)return false;auto* rows=cJSON_AddArrayToObject(j,"shares");bool ok=rows&&cJSON_AddStringToObject(j,"state",state);
 for(unsigned i=0;ok&&i<kShares;++i){const auto& s=settings.shares[i];auto* r=cJSON_CreateObject();
  ok=r&&cJSON_AddNumberToObject(r,"slot",i)&&cJSON_AddStringToObject(r,"label",s.label)&&cJSON_AddStringToObject(r,"root",s.root)&&cJSON_AddStringToObject(r,"server",s.connection.server)&&cJSON_AddStringToObject(r,"share",s.connection.share)&&cJSON_AddStringToObject(r,"user",s.connection.user)&&cJSON_AddStringToObject(r,"domain",s.connection.domain)&&cJSON_AddBoolToObject(r,"hasPassword",s.connection.password[0]);
  if(ok)cJSON_AddItemToArray(rows,r);else cJSON_Delete(r);
 }
 if(ok)ok=cJSON_PrintPreallocated(j,output,capacity,false);cJSON_Delete(j);return ok;
}
}
