#include "SmbArtwork.h"
#include "SmbWorkers.h"
#include "NetworkArtworkJpeg.h"
#include "NetworkEmbeddedArtwork.h"
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
#include <thread>
#include <chrono>
#endif
#include <new>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <initializer_list>
namespace SmbLibrary {
namespace {
void* allocate(size_t size){
#ifdef ESP_PLATFORM
 return heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
 return malloc(size);
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
struct Artwork::Task{Share share;char folder[kPath+1]{},track[kPath+1]{};uint32_t generation=0;uint16_t* pixels=nullptr;std::atomic<bool> cancel{false},done{false};};
bool Artwork::start(const Share& share,const char* folder,uint32_t generation,const char* track){
 if(task_||!folder||strlen(folder)>kPath||!track||strlen(track)>kPath)return false;void* memory=allocate(sizeof(Task));if(!memory)return false;
 task_=new(memory)Task;task_->share=share;strcpy(task_->folder,folder);strcpy(task_->track,track);task_->generation=generation;
 if(!SmbWorkers::submit(SmbWorkers::Kind::Artwork,run,task_)){task_->~Task();free(task_);task_=nullptr;return false;}return true;
}
void Artwork::run(void* argument){auto& t=*static_cast<Task*>(argument);
 auto load=[&](Client& client,uint64_t offset,size_t size)->uint16_t*{
  if(t.cancel||!size||size>NetworkArtwork::MaximumJpeg)return nullptr;
  auto* bytes=static_cast<uint8_t*>(allocate(size));if(!bytes)return nullptr;size_t at=0;
  while(at<size&&!t.cancel){const size_t wanted=std::min<size_t>(32768,size-at);auto n=client.readAt(offset+at,bytes+at,wanted);if(n<=0||size_t(n)>wanted)break;at+=n;idle();}
  client.close();auto* pixels=static_cast<uint16_t*>(allocate(112*112*2));
  if(!(pixels&&at==size&&!t.cancel&&decodeNetworkArtwork(bytes,size,pixels,t.cancel))){free(pixels);pixels=nullptr;}free(bytes);return pixels;
 };
 if(t.track[0]&&!t.cancel){Client client(t.cancel);Identity identity;
  if(client.connect(t.share.connection)&&client.open(t.track,identity)){
   auto read=[&](uint64_t offset,void* output,size_t size){
    if(t.cancel||offset>identity.size||size>identity.size-offset)return false;
    auto* bytes=static_cast<uint8_t*>(output);size_t at=0;
    while(at<size&&!t.cancel){const auto n=client.readAt(offset+at,bytes+at,size-at);if(n<=0||size_t(n)>size-at)return false;at+=n;}
    return at==size&&!t.cancel;
   };
   NetworkArtwork::Location location;if(NetworkArtwork::locate(read,identity.size,location))t.pixels=load(client,location.offset,location.length);
  }
 }
 for(const char* name:{"cover.jpg","folder.jpg","front.jpg","Cover.jpg","Folder.jpg","Front.jpg","cover.jpeg","Cover.jpeg"}){
  if(t.cancel||t.pixels)break;Client client(t.cancel);if(!client.connect(t.share.connection))break;char path[kPath+1];Identity identity;if(!join(t.folder,name,path)||!client.open(path,identity))continue;
  t.pixels=load(client,0,identity.size);
 }
 memset(&t.share,0,sizeof t.share);t.done.store(true,std::memory_order_release);
}
void Artwork::cancel(){if(task_)task_->cancel=true;}
bool Artwork::stopped()const{return !task_||(task_->done.load(std::memory_order_acquire)&&SmbWorkers::finished(SmbWorkers::Kind::Artwork,task_));}
uint16_t* Artwork::take(uint32_t& generation){if(!task_||!stopped()||!SmbWorkers::release(SmbWorkers::Kind::Artwork,task_))return nullptr;generation=task_->generation;auto* p=task_->pixels;task_->~Task();free(task_);task_=nullptr;return p;}
Artwork::~Artwork(){cancel();while(!stopped())idle();uint32_t generation;free(take(generation));}
}
