#include "SmbReadAhead.h"
#include "SmbWorkers.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
#include <thread>
#endif
namespace SmbLibrary {
namespace {
constexpr size_t block=32768;
void idle(){
#ifdef ESP_PLATFORM
 vTaskDelay(pdMS_TO_TICKS(2));
#else
 std::this_thread::sleep_for(std::chrono::milliseconds(2));
#endif
}
}
ReadAhead::ReadAhead(FileReader& upstream,const std::atomic<bool>& cancel,uint8_t* bytes,size_t capacity):
 upstream_(upstream),cancel_(cancel),bytes_(bytes),capacity_(capacity){}
ReadAhead::~ReadAhead(){stop();}
bool ReadAhead::start(uint64_t length){
 if(started_||cancel_||!bytes_||capacity_<2*block)return false;
 {std::lock_guard<std::mutex> lock(mutex_);length_=length;start_=head_=used_=0;failed_=false;++generation_;}
 stopping_=false;demandUs_=0;
 if(!SmbWorkers::submit(SmbWorkers::Kind::ReadAhead,run,this))return false;
 started_=true;return true;
}
void ReadAhead::stop(){
 if(!started_)return;stopping_=true;
 // Keep the object and cache alive until the pool publishes callback-return.
 while(!SmbWorkers::finished(SmbWorkers::Kind::ReadAhead,this))idle();
 SmbWorkers::release(SmbWorkers::Kind::ReadAhead,this);started_=false;
}
void ReadAhead::run(void* parameter){
 auto& s=*static_cast<ReadAhead*>(parameter);
 while(!s.stopping_&&!s.cancel_){
  uint64_t at=0,generation=0;size_t tail=0,wanted=0;
  {std::lock_guard<std::mutex> lock(s.mutex_);
   at=s.start_+s.used_;generation=s.generation_;tail=(s.head_+s.used_)%s.capacity_;
   if(!s.failed_&&at<s.length_)wanted=std::min({block,s.capacity_-s.used_,s.capacity_-tail,size_t(std::min<uint64_t>(s.length_-at,block))});
  }
  if(!wanted){idle();continue;}
  // This tail is reserved implicitly by the single I/O producer. A seek may
  // invalidate its generation but cannot publish/reuse it before this returns.
  const int count=s.upstream_.readAt(at,s.bytes_+tail,wanted);
  {std::lock_guard<std::mutex> lock(s.mutex_);
   if(s.stopping_||s.cancel_)break;
   if(generation!=s.generation_)continue;
   if(count<=0||size_t(count)>wanted){s.failed_=true;continue;}
   s.used_+=size_t(count);
  }
 }
}
int ReadAhead::readAt(uint64_t offset,uint8_t* bytes,size_t count){
 const auto before=std::chrono::steady_clock::now();
 auto measured=[&](int result){demandUs_+=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-before).count());return result;};
 if(!started_||!bytes||!count||count>block||offset>length_)return measured(-1);
 while(!cancel_&&!stopping_){
  {std::lock_guard<std::mutex> lock(mutex_);
   if(offset!=start_){
    if(offset>=start_&&offset-start_<=used_){size_t skipped=size_t(offset-start_);head_=(head_+skipped)%capacity_;used_-=skipped;start_=offset;}
    else{start_=offset;head_=used_=0;++generation_;}
   }
   if(used_){
    const size_t n=std::min(count,used_),first=std::min(n,capacity_-head_);
    memcpy(bytes,bytes_+head_,first);if(n>first)memcpy(bytes+first,bytes_,n-first);
    head_=(head_+n)%capacity_;start_+=n;used_-=n;return measured(int(n));
   }
   if(start_==length_)return measured(0);
   if(failed_)return measured(-1);
  }
  idle();
 }
 return measured(-1);
}
}
