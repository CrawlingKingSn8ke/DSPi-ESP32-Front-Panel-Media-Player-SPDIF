#include "SmbWorkers.h"
#include <mutex>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#else
#include <thread>
#include <condition_variable>
#endif
namespace SmbWorkers {
namespace {
struct Slot {
 std::mutex mutex;Function function=nullptr;void* job=nullptr;bool done=false,started=false;
#ifdef ESP_PLATFORM
 TaskHandle_t task=nullptr;
#else
 std::thread thread;std::condition_variable wake;bool shutdown=false;
 ~Slot(){ {std::lock_guard<std::mutex> lock(mutex);shutdown=true;}wake.notify_one();if(thread.joinable())thread.join();}
#endif
};
Slot* slot(Kind kind){const unsigned n=unsigned(kind);if(n>=4)return nullptr;static Slot slots[4];return &slots[n];}
void loop(void* parameter){
 auto& s=*static_cast<Slot*>(parameter);
 for(;;){
#ifdef ESP_PLATFORM
  ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
#endif
  Function function;void* job;
  {
   std::unique_lock<std::mutex> lock(s.mutex);
#ifndef ESP_PLATFORM
   s.wake.wait(lock,[&]{return s.shutdown||(s.job&&!s.done);});if(s.shutdown)return;
#endif
   if(!s.job||s.done)continue;function=s.function;job=s.job;
  }
  function(job);
  {std::lock_guard<std::mutex> lock(s.mutex);s.done=true;}
 }
}
}
bool submit(Kind kind,Function function,void* job){
 auto* s=slot(kind);if(!s||!function||!job)return false;
 std::lock_guard<std::mutex> lock(s->mutex);if(s->job)return false;
 if(!s->started){
#ifdef ESP_PLATFORM
  static const char* names[]={"smb-decode","smb-browser","smb-art","smb-read"};
  if(xTaskCreatePinnedToCoreWithCaps(loop,names[unsigned(kind)],32768,s,(kind==Kind::Audio||kind==Kind::ReadAhead)?3:1,&s->task,tskNO_AFFINITY,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS)return false;
#else
  try{s->thread=std::thread(loop,s);}catch(...){return false;}
#endif
  s->started=true;
 }
 s->function=function;s->job=job;s->done=false;
#ifdef ESP_PLATFORM
 xTaskNotifyGive(s->task);
#else
 s->wake.notify_one();
#endif
 return true;
}
bool finished(Kind kind,const void* job){auto* s=slot(kind);if(!s||!job)return false;std::lock_guard<std::mutex> lock(s->mutex);return s->job==job&&s->done;}
bool release(Kind kind,const void* job){auto* s=slot(kind);if(!s||!job)return false;std::lock_guard<std::mutex> lock(s->mutex);if(s->job!=job||!s->done)return false;s->job=nullptr;s->function=nullptr;s->done=false;return true;}
}
