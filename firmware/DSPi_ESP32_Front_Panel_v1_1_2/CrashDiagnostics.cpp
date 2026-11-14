#include "CrashDiagnostics.h"
#include <cstdio>
#include <cstdarg>
#include <algorithm>
#ifdef ESP_PLATFORM
#include <esp_core_dump.h>
#endif
namespace CrashDiagnostics {
namespace {
struct Writer {
 char* out;size_t capacity,used=0;bool okay=true;
 void add(const char* format,...){
  if(!okay)return;va_list args;va_start(args,format);const int n=vsnprintf(out+used,capacity-used,format,args);va_end(args);
  if(n<0||size_t(n)>=capacity-used){okay=false;return;}used+=size_t(n);
 }
};
}
bool formatJson(char* output,size_t capacity){
 if(!output||!capacity)return false;Writer w{output,capacity};
#if defined(ESP_PLATFORM) && CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
 const int imageStatus=esp_core_dump_image_check();
 esp_core_dump_summary_t summary{};const int summaryStatus=imageStatus==ESP_OK?esp_core_dump_get_summary(&summary):-1;
 const bool valid=imageStatus==ESP_OK&&summaryStatus==ESP_OK;
 w.add("{\"supported\":true,\"valid\":%s,\"imageStatus\":%d,\"summaryStatus\":%d",valid?"true":"false",imageStatus,summaryStatus);
 if(valid){
  char task[17]{},sha[65]{};
  for(size_t i=0;i<16&&summary.exc_task[i];++i){const unsigned char c=summary.exc_task[i];task[i]=(c>=32&&c<127&&c!='"'&&c!='\\')?char(c):'_';}
  const size_t shaLimit=std::min<size_t>(64,sizeof summary.app_elf_sha256);
  for(size_t i=0;i<shaLimit&&summary.app_elf_sha256[i];++i){const auto c=summary.app_elf_sha256[i];sha[i]=((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))?char(c):'_';}
  const unsigned depth=std::min<unsigned>(summary.exc_bt_info.depth,16);
  w.add(",\"task\":\"%s\",\"pc\":\"0x%08lx\",\"exceptionCause\":%lu,\"faultAddress\":\"0x%08lx\",\"recordedElfSha256\":\"%s\",\"backtraceCorrupted\":%s,\"backtraceTruncated\":%s,\"backtrace\":[",task,(unsigned long)summary.exc_pc,(unsigned long)summary.ex_info.exc_cause,(unsigned long)summary.ex_info.exc_vaddr,sha,summary.exc_bt_info.corrupted?"true":"false",summary.exc_bt_info.depth>16?"true":"false");
  for(unsigned i=0;i<depth;++i)w.add("%s\"0x%08lx\"",i?",":"",(unsigned long)summary.exc_bt_info.bt[i]);w.add("]");
 }
 w.add("}");
#else
 w.add("{\"supported\":false,\"valid\":false}");
#endif
 return w.okay;
}
}
