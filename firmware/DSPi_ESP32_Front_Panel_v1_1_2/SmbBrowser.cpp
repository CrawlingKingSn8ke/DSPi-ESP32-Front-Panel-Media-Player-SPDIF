#include "SmbBrowser.h"
#include "SmbWorkers.h"
#include <atomic>
#include <new>
#include <cstdlib>
#include <cstring>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
#include <thread>
#include <chrono>
#endif
namespace SmbLibrary {
namespace {
void* allocate(size_t bytes){
#ifdef ESP_PLATFORM
 return heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
 return malloc(bytes);
#endif
}
void idle(){
#ifdef ESP_PLATFORM
 vTaskDelay(1);
#else
 std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
}
}
struct Browser::Task{Share share;BrowseResponse* response=nullptr;std::atomic<bool> cancel{false},done{false};};
bool Browser::browse(const Share& share,const char* path,const char* after,uint32_t generation){
 char checked[kPath+1];if(task_||!generation||!share.id||!share.revision||!validShare(share)||!join(path,"probe",checked)||!after||strlen(after)>kName)return false;
 void* memory=allocate(sizeof(Task));void* response=allocate(sizeof(BrowseResponse));
 if(!memory||!response){free(memory);free(response);return false;}
 task_=new(memory)Task;task_->response=new(response)BrowseResponse;task_->share=share;
 auto& r=*task_->response;r.generation=generation;r.share=share.id;r.revision=share.revision;strcpy(r.path,path);strcpy(r.after,after);
 if(!SmbWorkers::submit(SmbWorkers::Kind::Browse,run,task_)){freeResponse(task_->response);task_->~Task();free(task_);task_=nullptr;return false;}
 return true;
}
void Browser::run(void* arg){
 auto& task=*static_cast<Task*>(arg);auto& r=*task.response;
 {Client client(task.cancel);r.okay=client.connect(task.share.connection)&&client.list(r.path,r.after,r.page);if(!r.okay)strncpy(r.error,client.error(),sizeof(r.error)-1);}
 r.cancelled=task.cancel.load();if(r.cancelled)r.okay=false;
 memset(task.share.connection.password,0,sizeof(task.share.connection.password));task.done.store(true,std::memory_order_release);
}
void Browser::cancel(){if(task_)task_->cancel=true;}
bool Browser::busy()const{return !stopped();}
bool Browser::stopped()const{return !task_||(task_->done.load(std::memory_order_acquire)&&SmbWorkers::finished(SmbWorkers::Kind::Browse,task_));}
BrowseResponse* Browser::take(){
 if(!task_||!stopped()||!SmbWorkers::release(SmbWorkers::Kind::Browse,task_))return nullptr;
 auto* response=task_->response;task_->~Task();free(task_);task_=nullptr;return response;
}
void Browser::freeResponse(BrowseResponse* response){if(response){response->~BrowseResponse();free(response);}}
Browser::~Browser(){cancel();while(busy())idle();freeResponse(take());}
}
