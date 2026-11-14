#include "SmbAudioJob.h"
#include "SmbWorkers.h"
#include "SmbReadAhead.h"
#include "NetworkPlaybackPolicy.h"
#include <chrono>
#include <new>
#include <cstdlib>
#include <cstring>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
#include <chrono>
#include <thread>
#endif
namespace SmbLibrary {
namespace {
constexpr size_t compressedReadAheadBytes=512*1024;
uint32_t nowUs(){return uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
void maximum(std::atomic<uint32_t>& target,uint32_t value){auto old=target.load();while(old<value&&!target.compare_exchange_weak(old,value)){}}
void idle(){
#ifdef ESP_PLATFORM
 vTaskDelay(1);
#else
 std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
}
void* allocate(size_t n){
#ifdef ESP_PLATFORM
 return heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
 return malloc(n);
#endif
}
}
struct AudioJob::State {
 std::atomic<bool> cancelled{false},ready{false},failed{false},done{false};
 std::atomic<uint32_t> token{0};uint32_t generation=0;
 Credentials credentials;char path[kPath+1]={0},error[96]={0};
 Identity identity,expected;bool verify=false;uint64_t firstFrame=0;
 uint8_t* cache;uint8_t* compressed;Client client;ReadAhead readAhead;ByteSource source;NetworkAudioDecoder decoder;NetworkAudioInfo info;
 NetworkPcmSink& sink;
 std::atomic<uint32_t> decodeCalls{0},decodeMaxUs{0},computeMaxUs{0};std::atomic<uint64_t> decodedFrames{0};
 State(uint8_t* bytes,uint8_t* ahead,NetworkPcmSink& output):cache(bytes),compressed(ahead),client(cancelled),readAhead(client,cancelled,ahead,compressedReadAheadBytes),source(readAhead,cancelled,bytes,32768),sink(output){}
 void fail(const char* text){strncpy(error,text,sizeof(error)-1);failed=true;}
};
bool AudioJob::start(const Credentials& credentials,const char* path,uint32_t generation,NetworkPcmSink& sink,uint64_t firstFrame,const Identity* expected){
 char checked[kPath+1];if(state_||!generation||!path||!*path||!join(path,"probe",checked))return false;
 auto* cache=static_cast<uint8_t*>(allocate(32768));auto* compressed=static_cast<uint8_t*>(allocate(compressedReadAheadBytes));void* memory=allocate(sizeof(State));
 if(!cache||!compressed||!memory){free(cache);free(compressed);free(memory);return false;}
 state_=new(memory)State(cache,compressed,sink);state_->credentials=credentials;strcpy(state_->path,path);
 state_->generation=generation;state_->firstFrame=firstFrame;if(expected){state_->verify=true;state_->expected=*expected;}
 if(!SmbWorkers::submit(SmbWorkers::Kind::Audio,run,state_)){state_->~State();free(state_);free(cache);free(compressed);state_=nullptr;return false;}
 return true;
}
void AudioJob::run(void* p){
 auto& s=*static_cast<State*>(p);
 bool okay=s.client.connect(s.credentials)&&s.client.open(s.path,s.identity);
 memset(s.credentials.password,0,sizeof(s.credentials.password));
 if(!okay&&!s.cancelled)s.fail(s.client.error());
 if(okay&&s.verify){
  auto& a=s.identity;auto& b=s.expected;
  if(a.inode!=b.inode||a.size!=b.size||a.modified!=b.modified||a.nanoseconds!=b.nanoseconds){okay=false;s.fail("File changed on server");}
 }
 if(okay){
  okay=s.readAhead.start(s.identity.size);
  if(!okay&&!s.cancelled)s.fail("Network reader unavailable");
 }
 if(okay){
  s.source.reset(s.identity.size);
  okay=s.decoder.open(s.source,idle);
  if(okay&&s.firstFrame)okay=s.decoder.seekFrame(s.firstFrame);
  if(!okay&&!s.cancelled)s.fail("Unsupported or unreadable audio");
 }
 if(okay&&!s.cancelled){
  s.info=s.decoder.info();s.ready.store(true,std::memory_order_release);
  while(!s.cancelled&&!s.token.load())idle();
  const uint32_t token=s.token.load();int32_t pcm[1024];NetworkPlayback::WorkBudget budget;budget.reset(nowUs());
  while(token&&!s.cancelled&&s.token.load()==token){
   const uint64_t before=s.readAhead.demandMicroseconds();const uint32_t started=nowUs();
   const size_t frames=s.decoder.decode(pcm,512);
   const uint32_t elapsed=nowUs()-started;const uint64_t readUs=s.readAhead.demandMicroseconds()-before;
   ++s.decodeCalls;s.decodedFrames+=frames;maximum(s.decodeMaxUs,elapsed);maximum(s.computeMaxUs,elapsed-uint32_t(std::min<uint64_t>(elapsed,readUs)));
   if(!frames){
    const bool bad=s.decoder.failed();if(bad&&!s.cancelled)s.fail("Network file read failed");
    if(!s.cancelled)s.sink.finish(token,bad);break;
   }
   size_t written=0;
   while(written<frames&&!s.cancelled&&s.token.load()==token){
    const size_t count=s.sink.write(token,pcm+written*2,frames-written);
    if(count>frames-written){s.fail("Audio producer rejected data");s.cancelled=true;break;}
    written+=count;if(!count)idle();
   }
   if(!s.cancelled&&budget.due(nowUs())){idle();budget.reset(nowUs());}
  }
 }
 // Cancel wakes any in-flight SMB wait before joining the reader. Client and
 // all cache memory outlive its callback-return barrier, including on EOF.
 s.cancelled=true;s.decoder.close();s.readAhead.stop();s.client.close();
 // The pool additionally publishes callback-return before release can free it.
 s.done.store(true,std::memory_order_release);
}
AudioJob::~AudioJob(){cancel();while(!stopped())idle();release();}
bool AudioJob::ready()const{return state_&&state_->ready.load(std::memory_order_acquire);}
bool AudioJob::failed()const{return state_&&state_->failed;}
bool AudioJob::stopped()const{return !state_||(state_->done.load(std::memory_order_acquire)&&SmbWorkers::finished(SmbWorkers::Kind::Audio,state_));}
NetworkAudioInfo AudioJob::info()const{return ready()?state_->info:NetworkAudioInfo{};}
Identity AudioJob::identity()const{return ready()?state_->identity:Identity{};}
AudioTelemetry AudioJob::telemetry()const{
 if(!state_)return {};return {state_->client.readStatistics(),state_->decodeCalls.load(),state_->decodeMaxUs.load(),state_->computeMaxUs.load(),state_->decodedFrames.load(),uint32_t(state_->readAhead.bufferedBytes()),uint32_t(state_->readAhead.capacityBytes())};
}
uint32_t AudioJob::generation()const{return state_?state_->generation:0;}
MediaByteSource* AudioJob::source()const{return state_?&state_->source:nullptr;}
const char* AudioJob::error()const{return state_&&state_->failed?state_->error:"";}
void AudioJob::supplyPcmToken(uint32_t value){if(state_&&!state_->cancelled)state_->token=value;}
void AudioJob::cancel(){if(state_){state_->token=0;state_->cancelled=true;}}
bool AudioJob::release(){
 if(!state_)return true;if(!stopped()||state_->token.load())return false;
 if(!SmbWorkers::release(SmbWorkers::Kind::Audio,state_))return false;
 auto* cache=state_->cache;auto* compressed=state_->compressed;state_->~State();free(state_);free(cache);free(compressed);state_=nullptr;return true;
}
}
