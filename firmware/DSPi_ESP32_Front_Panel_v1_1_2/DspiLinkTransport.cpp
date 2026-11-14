#include "DspiLinkTransport.h"
#include <Arduino.h>
#include <HardwareSerial.h>
#include <esp_heap_caps.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

namespace DspiLink {
namespace {
struct Notice { uint16_t length;uint8_t bytes[132]; };
struct Result { dspi_reply_t wire{};uint32_t tag=0;bool ready=false;uint8_t bytes[8192]; };
struct State {
  Broker broker;
  dspi_link_t link{};
  uint8_t parser[DSPI_PARSER_BUFFER_BYTES(8192)];
  Result local,network;
  TransportStatus status;
  HardwareSerial* serial=nullptr;
  SemaphoreHandle_t mutex=nullptr,localDone=nullptr;
  QueueHandle_t notices=nullptr;
  TaskHandle_t worker=nullptr;
  bool stopping=false;
  uint32_t localJob=0;
  uint32_t networkGenerations[2]{};
  bool abandonedLocal=false;
  bool localWanted=false;
  uint32_t localWindowUntil=0;
};
State* state=nullptr;
void lock(){xSemaphoreTake(state->mutex,portMAX_DELAY);}
void unlock(){xSemaphoreGive(state->mutex);}
bool portWrite(void*,const uint8_t* bytes,size_t size){
  if(state->serial->write(bytes,size)!=size)return false;
  state->serial->flush();return true;
}
size_t portRead(void*,uint8_t* bytes,size_t capacity,uint32_t timeout){
  const uint32_t start=millis();
  do {
    size_t count=0;
    while(count<capacity&&state->serial->available())bytes[count++]=uint8_t(state->serial->read());
    if(count)return count;
    if(!timeout)break;
    vTaskDelay(1);
  }while(uint32_t(millis()-start)<timeout);
  return 0;
}
bool portBaud(void*,uint32_t baud){state->serial->updateBaudRate(baud);return true;}
bool portRestart(void*){
  const uint32_t baud=dspi_link_baud(&state->link);
  state->serial->end();vTaskDelay(pdMS_TO_TICKS(20));
  state->serial->setRxBufferSize(4096);
  state->serial->begin(baud,SERIAL_8N1,16,17);return true;
}
uint32_t portTime(void*){return millis();}
void onNotice(void*,const uint8_t* packet,size_t size){
  if(size>132){lock();++state->status.droppedNotifications;unlock();return;}
  Notice n{};n.length=uint16_t(size);memcpy(n.bytes,packet,size);
  if(xQueueSend(state->notices,&n,0)!=pdTRUE){lock();++state->status.droppedNotifications;unlock();}
}
void storeResult(Result& out,const dspi_reply_t& reply,uint32_t tag){
  out.wire=reply;out.tag=tag;
  if(reply.length>sizeof(out.bytes)){out.wire.kind=DSPI_REPLY_CORRUPT;out.wire.length=0;}
  if(out.wire.length)memcpy(out.bytes,reply.payload,out.wire.length);
  out.wire.payload=out.bytes;out.ready=true;
}
void worker(void*){
  dspi_port_t port={nullptr,portWrite,portRead,portBaud,portRestart,portTime};
  dspi_link_init(&state->link,&port,115200,state->parser,sizeof(state->parser),onNotice,nullptr);
  uint32_t nextProbe=0,nextIdentity=0;
  while(true){
    lock();const bool stop=state->stopping;
    // The completion buffer is not reused until its owner has copied it.
    const bool panelWindow=int32_t(state->localWindowUntil-millis())>0;
    auto* work=(!state->network.ready&&!state->local.ready&&!stop)?state->broker.take(panelWindow):nullptr;
    const bool present=state->status.present;
    const bool stale=work&&work->owner==LinkOwner::Network&&work->session<2&&work->sessionGeneration!=state->networkGenerations[work->session];
    unlock();
    if(stop){
      dspi_link_listen(&state->link,20);
      lock();state->status.running=false;state->worker=nullptr;unlock();
      vTaskDelete(nullptr);return;
    }
    if(work){
      const dspi_reply_t reply=stale?dspi_reply_t{DSPI_REPLY_NO_DEVICE,0,nullptr,0,0}:dspi_link_transact(&state->link,&work->request,work->mayRetry);
      // Responses have no request ID. A failed operation cannot leave a
      // delayed same-shaped ACK waiting for the next user's request.
      if(!stale&&reply.kind!=DSPI_REPLY_ANSWERED)dspi_link_listen(&state->link,650);
      lock();
      if(work->owner==LinkOwner::Panel)storeResult(state->local,reply,0);
      else storeResult(state->network,reply,work->job);
      state->status.counters=*dspi_link_counters(&state->link);
      if(reply.kind==DSPI_REPLY_ANSWERED)state->status.present=true;
      else if(dspi_link_failures_in_a_row(&state->link)>=3)state->status.present=false;
      state->broker.finish(work->ticket);state->status.busy=false;
      const bool local=work->owner==LinkOwner::Panel;
      if(!local&&state->localWanted){state->localWindowUntil=millis()+25;state->localWanted=false;}
      unlock();if(local)xSemaphoreGive(state->localDone);
    }else if(!present&&int32_t(millis()-nextProbe)>=0){
      static const uint32_t rates[]=DSPI_UART_BAUD_CANDIDATES;
      dspi_identity_t identity{};
      const bool found=dspi_link_find_baud(&state->link,dspi_link_baud(&state->link),rates,sizeof(rates)/sizeof(*rates))&&dspi_link_read_identity(&state->link,&identity);
      lock();state->status.present=found;state->status.baud=dspi_link_baud(&state->link);if(found){state->status.identity=identity;++state->status.generation;}unlock();
      nextProbe=millis()+2000;nextIdentity=millis()+2000;
    }else if(present&&int32_t(millis()-nextIdentity)>=0){
      // A working wire is not proof that the same DSPi/configuration remains.
      // Re-read identity while idle, using the same sole UART owner.
      dspi_identity_t identity{};bool found=dspi_link_read_identity(&state->link,&identity);
      lock();
      if(found){
        if(memcmp(&identity,&state->status.identity,sizeof(identity)))++state->status.generation;
        state->status.identity=identity;
      }else if(dspi_link_failures_in_a_row(&state->link)>=3)state->status.present=false;
      state->status.baud=dspi_link_baud(&state->link);unlock();nextIdentity=millis()+2000;
    }else{dspi_link_listen(&state->link,1);vTaskDelay(1);}
  }
}
}
bool beginTransport(HardwareSerial& serial){
  if(state){if(transportRunning())return true;if(!finishTransportStop())return false;}
  state=new(std::nothrow) State();if(!state)return false;
  state->mutex=xSemaphoreCreateMutex();state->localDone=xSemaphoreCreateBinary();
  state->notices=xQueueCreate(24,sizeof(Notice));state->serial=&serial;
  if(!state->mutex||!state->localDone||!state->notices){
    if(state->mutex)vSemaphoreDelete(state->mutex);if(state->localDone)vSemaphoreDelete(state->localDone);if(state->notices)vQueueDelete(state->notices);
    delete state;state=nullptr;return false;
  }
  serial.end();serial.setRxBufferSize(4096);serial.begin(115200,SERIAL_8N1,16,17);
  state->status.running=true;
  if(xTaskCreate(worker,"dspi-link-uart",4096,nullptr,2,&state->worker)!=pdPASS){
    vSemaphoreDelete(state->mutex);vSemaphoreDelete(state->localDone);vQueueDelete(state->notices);
    delete state;state=nullptr;return false;
  }
  return true;
}
bool transportRunning(){if(!state)return false;lock();bool run=state->status.running;unlock();return run;}
bool transportOwnsUart(){return state!=nullptr;}
bool finishTransportStop(){
  if(!state)return true;
  lock();const bool ready=state->stopping&&!state->status.running;unlock();
  if(!ready)return false;
  // Worker has performed its final UART drain and makes no further State or
  // serial accesses after publishing running=false under the mutex.
  auto* serial=state->serial;
  vQueueDelete(state->notices);vSemaphoreDelete(state->localDone);vSemaphoreDelete(state->mutex);
  delete state;state=nullptr;
  serial->end();serial->setRxBufferSize(256);serial->begin(115200,SERIAL_8N1,16,17);
  return true;
}
LinkAdmission submit(const TransportRequest& request,LinkTicket& ticket){
  if(!state)return LinkAdmission::Offline;
  lock();LinkAdmission result=state->stopping?LinkAdmission::Offline:state->broker.submit(request,ticket);unlock();return result;
}
LinkAdmission executeLocal(const dspi_request_t& request,bool retry,uint8_t* bytes,uint16_t capacity,dspi_reply_t& reply){
  if(!state)return LinkAdmission::Offline;
  lock();
  if(state->stopping||!state->status.present){unlock();return LinkAdmission::Offline;}
  if(state->broker.active()||state->network.ready||state->local.ready){
    state->localWanted=true;
    if(state->network.ready)state->localWindowUntil=millis()+25;
    unlock();return LinkAdmission::Busy;
  }
  TransportRequest input{};input.request=request;input.owner=LinkOwner::Panel;input.mayRetry=retry;input.job=state->localJob;
  LinkTicket ticket;auto result=state->broker.submit(input,ticket);
  if(result==LinkAdmission::Accepted){state->localWanted=false;state->localWindowUntil=0;}
  unlock();
  if(result!=LinkAdmission::Accepted)return result;
  // Only this caller's legacy-sized operation is waited for. Worker storage
  // is retained even if the UART is noisy; no caller stack pointer is queued.
  if(xSemaphoreTake(state->localDone,pdMS_TO_TICKS(2300))!=pdTRUE){
    lock();state->abandonedLocal=true;unlock();
    reply={DSPI_REPLY_TIMEOUT,0,nullptr,0,1};return LinkAdmission::Accepted;
  }
  lock();reply=state->local.wire;
  if(reply.length>capacity){reply.kind=DSPI_REPLY_CORRUPT;reply.length=0;}
  if(reply.length)memcpy(bytes,state->local.bytes,reply.length);
  reply.payload=bytes;state->local.ready=false;unlock();return result;
}
void serviceTransport(NotificationHandler notification,ReplyHandler remote){
  if(!state)return;
  lock();if(state->abandonedLocal&&state->local.ready){state->local.ready=false;state->abandonedLocal=false;xSemaphoreTake(state->localDone,0);}unlock();
  Notice n;for(unsigned i=0;i<24&&xQueueReceive(state->notices,&n,0)==pdTRUE;++i)if(notification)notification(n.bytes,n.length);
  // Only the main loop calls this; the worker cannot reuse bytes until ready
  // is cleared, after the synchronous consumer has copied them.
  lock();const bool ready=state->network.ready;auto reply=state->network.wire;uint32_t tag=state->network.tag;unlock();
  if(ready){if(remote)remote(tag,reply);lock();state->network.ready=false;unlock();}
}
TransportStatus transportStatus(){if(!state)return {};lock();auto s=state->status;s.busy=state->broker.busy();unlock();return s;}
void localJob(uint32_t job){if(!state)return;lock();state->localJob=job;state->broker.setLocalJob(job);unlock();}
void networkSession(uint16_t slot,uint32_t generation){if(!state||slot>=2)return;lock();state->networkGenerations[slot]=generation;unlock();}
void requestStop(){if(!state)return;lock();state->stopping=true;unlock();}
bool stopped(){return !state||!transportRunning();}
}
