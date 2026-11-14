#include "SmbSettings.h"
#include <Preferences.h>
#include <mutex>
#include <new>
#include <cstdlib>
#include <cstring>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif
namespace SmbLibrary {
namespace {
std::mutex settingsMutex;
template<class T>struct Scratch {
 T* p=nullptr;
 Scratch(){
#ifdef ESP_PLATFORM
  void* bytes=heap_caps_malloc(sizeof(T),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
  void* bytes=malloc(sizeof(T));
#endif
  if(bytes)p=new(bytes)T{};
 }
 ~Scratch(){if(p){p->~T();volatile uint8_t* b=(volatile uint8_t*)p;for(size_t i=0;i<sizeof(T);++i)b[i]=0;free(p);}}
};
struct ReadWork{uint8_t bytes[kSettingsBytes];Settings candidate;};
bool loadUnlocked(Settings& result){
 Preferences nvs;if(!nvs.begin("dspi_smb",true)){result=Settings{};return true;}
 Scratch<ReadWork> work;if(!work.p){nvs.end();return false;}
 bool any=false,found=false;
 for(const char* key:{"a","b"}){
  if(!nvs.isKey(key))continue;any=true;const size_t n=nvs.getBytesLength(key);
  if(n<18||n>kSettingsBytes||nvs.getBytes(key,work.p->bytes,n)!=n||!decodeSettings(work.p->bytes,n,work.p->candidate))continue;
  if(!found||work.p->candidate.sequence>result.sequence){result=work.p->candidate;found=true;}
 }
 nvs.end();if(!any){result=Settings{};return true;}return found;
}
struct SaveWork{Settings current,draft;uint8_t encoded[kSettingsBytes],verify[kSettingsBytes];};
}
bool loadSettings(Settings& result){std::lock_guard<std::mutex> lock(settingsMutex);return loadUnlocked(result);}
bool saveSettings(Settings& settings){
 std::lock_guard<std::mutex> lock(settingsMutex);Scratch<SaveWork> work;if(!work.p)return false;
 auto& w=*work.p;if(!loadUnlocked(w.current)||w.current.sequence!=settings.sequence||settings.sequence==UINT32_MAX)return false;
 w.draft=settings;++w.draft.sequence;size_t length=encodeSettings(w.draft,w.encoded,sizeof(w.encoded));if(!length)return false;
 Preferences nvs;if(!nvs.begin("dspi_smb",false))return false;
 const char* key=(w.current.sequence&1)?"b":"a";
 bool okay=nvs.putBytes(key,w.encoded,length)==length&&nvs.getBytesLength(key)==length&&nvs.getBytes(key,w.verify,length)==length&&!memcmp(w.encoded,w.verify,length);
 nvs.end();if(okay)settings.sequence=w.draft.sequence;return okay;
}
}
