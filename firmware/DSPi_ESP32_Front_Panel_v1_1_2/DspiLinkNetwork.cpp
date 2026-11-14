#include "DspiLinkNetwork.h"
#include "DspiLinkWebSocket.h"
#include "DspiLinkSetup.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha1.h>
#include <mbedtls/base64.h>
#include <lwip/sockets.h>
#include <lwip/tcp.h>
#include <fcntl.h>
#include <errno.h>
#include <new>
#include <freertos/semphr.h>
#include <freertos/queue.h>
extern "C" {
#include "src/dspi_link_vendor/ws_header.h"
}
namespace DspiLink {
namespace {
constexpr unsigned Peers=5;
constexpr unsigned LinkNetworkStack=6144;
constexpr unsigned SetupNetworkStack=12288;
struct Out {SocketToken token;size_t size=0,sent=0;uint8_t opcode=0;uint8_t header[10];size_t headerSize=0;uint8_t* bytes=nullptr;};
struct Peer {
  int fd=-1;SocketToken token{};uint32_t address=0,opened=0,frameStart=0,closeAt=0;
  char http[2049]{};size_t httpSize=0;
  uint8_t setupBody[SetupBodyLimit+1]{};size_t setupExpected=0,setupRead=0;
  uint8_t* input=nullptr;WsDecoder* decoder=nullptr;
  bool web=false,upgrading=false,closedPending=false;
  bool httpFinished=false;uint32_t httpFinishedAt=0;
  uint16_t closeCode=0;
  Out* outgoing[32]{};unsigned head=0,count=0;
  char response[320]{};size_t responseSize=0,responseSent=0;
  const uint8_t* body=nullptr;size_t bodySize=0,bodySent=0;bool ownedBody=false;
};
struct Network {
  SocketSlots slots;Peer peers[Peers];
  SemaphoreHandle_t mutex=nullptr;QueueHandle_t events=nullptr,outgoing=nullptr;
  TaskHandle_t task=nullptr;int listener=-1;bool stop=false,done=false;
  char hostname[64]{},ip[16]{},info[4096]{};AssetLookup assets=nullptr;
  SetupHandler setup=nullptr;
};
Network* net=nullptr;
// Reconstruct directly in the existing heap-backed slot. Assigning Peer()
// creates a multi-KiB temporary on the small FreeRTOS network task stack.
void resetPeer(Peer& peer){peer.~Peer();::new(static_cast<void*>(&peer)) Peer();}
void lock(){xSemaphoreTake(net->mutex,portMAX_DELAY);}void unlock(){xSemaphoreGive(net->mutex);}
void* alloc(size_t bytes){void* p=heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);return p?p:malloc(bytes);}
bool same(SocketToken a,SocketToken b){return a.slot==b.slot&&a.generation==b.generation;}
void freeOut(Out* out){if(!out)return;lock();net->slots.consumed(out->token,out->size,millis());unlock();free(out->bytes);delete out;}
bool event(NetworkEventKind kind,Peer& peer,const uint8_t* data=nullptr,size_t size=0){
  const bool ordinary=kind==NetworkEventKind::Text||kind==NetworkEventKind::Binary||kind==NetworkEventKind::Activity;
  if(ordinary&&uxQueueSpacesAvailable(net->events)<=4)return false;
  if(ordinary){lock();bool room=net->slots.inbound(peer.token,size);unlock();if(!room)return false;}
  NetworkEvent e{};e.kind=kind;e.token=peer.token;e.address=peer.address;e.length=size;
  if(size){e.data=(uint8_t*)alloc(size+1);if(!e.data){if(ordinary){lock();net->slots.handled(peer.token,size);unlock();}return false;}memcpy(e.data,data,size);e.data[size]=0;}
  if(xQueueSend(net->events,&e,0)!=pdTRUE){free(e.data);if(ordinary){lock();net->slots.handled(peer.token,size);unlock();}return false;}return true;
}
void drop(Peer& p){
  if(p.fd>=0){shutdown(p.fd,SHUT_RDWR);::close(p.fd);p.fd=-1;}
  if(p.ownedBody){free((void*)p.body);p.ownedBody=false;}
  p.body=nullptr;
  for(unsigned i=0;i<p.count;++i)freeOut(p.outgoing[(p.head+i)%32]);p.count=0;
  if(p.token.valid()){
    lock();net->slots.close(p.token);unlock();p.closedPending=true;
  }else{delete p.decoder;free(p.input);resetPeer(p);}
}
void response(Peer& p,unsigned status,const char* type,const uint8_t* body,size_t bytes,bool gzip=false){
  const char* reason=status==200?"OK":status==403?"Forbidden":status==404?"Not Found":status==413?"Content Too Large":status==415?"Unsupported Media Type":status==429?"Too Many Requests":status==431?"Request Header Fields Too Large":status==503?"Service Unavailable":"Bad Request";
  p.responseSize=snprintf(p.response,sizeof(p.response),"HTTP/1.1 %u %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n%s\r\n",status,reason,type,unsigned(bytes),gzip?"Content-Encoding: gzip\r\n":"");
  p.body=body;p.bodySize=bytes;
}
bool upgradeReply(Peer& p,const Upgrade& request){
  char raw[61];snprintf(raw,sizeof(raw),"%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11",request.key);
  uint8_t hash[20],accept[32];size_t size=0;
  if(mbedtls_sha1((const uint8_t*)raw,strlen(raw),hash)!=0||mbedtls_base64_encode(accept,sizeof(accept),&size,hash,sizeof(hash))!=0)return false;
  accept[size]=0;
  p.responseSize=snprintf(p.response,sizeof(p.response),"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\nSec-WebSocket-Protocol: dspi-link-1\r\n\r\n",accept);
  p.upgrading=true;return true;
}
void request(Peer& p){
  if(net->setup){
    if(!strncmp(p.http,"PUT ",4)){
      const unsigned status=validateSetup(p.http,net->ip,net->hostname,p.setupExpected);
      if(status!=200){p.setupExpected=0;response(p,status,"text/plain",nullptr,0);}
      return;
    }
    if(strncmp(p.http,"GET / HTTP/1.1\r\n",16)&&strncmp(p.http,"GET /index.html HTTP/1.1\r\n",26)&&
       strncmp(p.http,"GET /dspi/panel/networks HTTP/1.1\r\n",33)&&strncmp(p.http,"GET /dspi/panel/rescan HTTP/1.1\r\n",31)&&
       strncmp(p.http,"GET /dspi/panel/setup-status HTTP/1.1\r\n",37)&&strncmp(p.http,"GET /dspi/panel/smb/",20)){
      response(p,404,"text/plain",nullptr,0);return;
    }
  }
  if(!strncmp(p.http,"GET /dspi/v1 HTTP/1.1\r\n",23)){
    Upgrade upgrade{};lock();SocketToken token=net->slots.open(millis());unlock();
    const unsigned status=validateUpgrade(p.http,net->ip,net->hostname,token.valid(),upgrade);
    if(status!=101){
      if(token.valid()){lock();net->slots.close(token);net->slots.release(token);unlock();}
      response(p,status,"text/plain",nullptr,0);return;
    }
    p.token=token;
    p.input=(uint8_t*)alloc(LinkFrameLimit+1);
    if(p.input)p.decoder=new(std::nothrow) WsDecoder(p.input,LinkFrameLimit);
    if(!p.input||!p.decoder||!upgradeReply(p,upgrade)){drop(p);return;}
    return;
  }
  char path[128]{};
  if(sscanf(p.http,"GET %127s HTTP/1.1",path)!=1||strchr(path,'?')){response(p,400,"text/plain",nullptr,0);return;}
  if(!strcmp(path,"/dspi/v1/info")){
    lock();size_t n=strlen(net->info);auto* copy=(uint8_t*)alloc(n);if(copy)memcpy(copy,net->info,n);unlock();
    if(!copy){response(p,503,"text/plain",nullptr,0);return;}
    p.ownedBody=true;response(p,200,"application/json",copy,n);return;
  }
  auto asset=net->assets?net->assets(path):NetworkAsset{};
  if(asset.data&&asset.snapshot){
    if(asset.size>8192){response(p,503,"text/plain",nullptr,0);return;}
    auto* copy=(uint8_t*)alloc(asset.size);
    if(!copy){response(p,503,"text/plain",nullptr,0);return;}
    memcpy(copy,asset.data,asset.size);asset.data=copy;p.ownedBody=true;
  }
  response(p,asset.data?200:404,asset.type,asset.data,asset.size,asset.gzip);
}
bool writeSome(int fd,const uint8_t* bytes,size_t size,size_t& sent){
  if(sent==size)return true;
  const int n=send(fd,bytes+sent,std::min(size-sent,size_t(1024)),MSG_DONTWAIT);
  if(n>0){sent+=n;return true;}return n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR);
}
void pump(Peer& p){
  if(p.httpFinished){
    // A rejected upload may still be arriving. Half-close after the error
    // response and drain briefly so unread bytes cannot reset that response.
    uint8_t discard[512];int n=recv(p.fd,discard,sizeof(discard),MSG_DONTWAIT);
    if(n==0||uint32_t(millis()-p.httpFinishedAt)>=200)drop(p);
    return;
  }
  if(p.responseSize){
    if(!writeSome(p.fd,(uint8_t*)p.response,p.responseSize,p.responseSent)){drop(p);return;}
    if(p.responseSent!=p.responseSize)return;
    if(p.upgrading){p.upgrading=false;p.web=true;p.responseSize=0;if(!event(NetworkEventKind::Open,p))drop(p);return;}
    if(p.bodySize&&!writeSome(p.fd,p.body,p.bodySize,p.bodySent)){drop(p);return;}
    if(p.bodySent==p.bodySize){shutdown(p.fd,SHUT_WR);p.httpFinished=true;p.httpFinishedAt=millis();}return;
  }
  if(p.count){
    auto* out=p.outgoing[p.head];
    size_t headerSent=std::min(out->sent,out->headerSize);
    if(headerSent<out->headerSize){
      size_t old=headerSent;if(!writeSome(p.fd,out->header,out->headerSize,headerSent)){drop(p);return;}out->sent+=headerSent-old;
      if(headerSent>old){lock();net->slots.progress(p.token,millis());unlock();}
    }
    if(out->sent>=out->headerSize){
      size_t dataSent=out->sent-out->headerSize,old=dataSent;
      if(out->size&&!writeSome(p.fd,out->bytes,out->size,dataSent)){drop(p);return;}out->sent+=dataSent-old;
      if(dataSent>old){lock();net->slots.progress(p.token,millis());unlock();}
    }
    if(out->sent==out->headerSize+out->size){freeOut(out);p.head=(p.head+1)%32;--p.count;}
  }
  if(p.closeCode&&(!p.count||uint32_t(millis()-p.closeAt)>=1000)){
    // Never insert a close frame into a partially written data frame.
    if(!p.count){const uint8_t close[]={0x88,2,uint8_t(p.closeCode>>8),uint8_t(p.closeCode)};send(p.fd,close,sizeof(close),MSG_DONTWAIT);}
    drop(p);
  }
}
void received(Peer& p,const uint8_t* bytes,size_t count){
  for(size_t i=0;i<count&&p.fd>=0;++i){
    if(!p.web){
      if(p.responseSize)return;
      if(p.setupExpected){
        p.setupBody[p.setupRead++]=bytes[i];
        if(p.setupRead==p.setupExpected){
          p.setupBody[p.setupRead]=0;
          const unsigned status=net->setup(p.address,p.setupBody,p.setupRead);
          memset(p.setupBody,0,sizeof(p.setupBody));p.setupExpected=0;
          static const uint8_t ok[]="Saved. Reconnect your phone to home Wi-Fi. The ESP will now try to join it; check Connection on the panel.";
          response(p,status,"text/plain",status==200?ok:nullptr,status==200?sizeof(ok)-1:0);
          return;
        }
        continue;
      }
      if(!bytes[i]||p.httpSize>=LinkHttpLimit){response(p,431,"text/plain",nullptr,0);return;}
      p.http[p.httpSize++]=char(bytes[i]);p.http[p.httpSize]=0;
      if(p.httpSize>=4&&!memcmp(p.http+p.httpSize-4,"\r\n\r\n",4)){request(p);if(!p.setupExpected)return;}
    }else{
      if(p.closeCode)return;
      if(!p.decoder->partial())p.frameStart=millis();
      const auto result=p.decoder->feed(bytes[i]);
      if(result==WsEvent::Error){p.closeCode=p.decoder->error();p.closeAt=millis();return;}
      if(result==WsEvent::Frame){
        const uint8_t op=p.decoder->opcode();const size_t n=p.decoder->length();
        if(op==1||op==2){
          p.input[n]=0;
          if(op==1&&(!jsonDepthAllowed((const char*)p.input)||memchr(p.input,0,n))){p.closeCode=1007;p.closeAt=millis();return;}
          if(!event(op==1?NetworkEventKind::Text:NetworkEventKind::Binary,p,p.input,n)){p.closeCode=1013;p.closeAt=millis();return;}
        }else if(op==8){p.closeCode=1000;p.closeAt=millis();return;}
        else {event(NetworkEventKind::Activity,p);if(op==9)sendNetwork(p.token,10,p.input,n);}
        p.decoder->reset();
      }
    }
  }
}
void task(void*){
  while(true){
    lock();bool stop=net->stop;unlock();if(stop)break;
    sockaddr_in address{};socklen_t size=sizeof(address);
    const int fd=accept(net->listener,(sockaddr*)&address,&size);
    if(fd>=0){
      Peer* free=nullptr;for(auto& p:net->peers)if(p.fd<0&&!p.token.valid()){free=&p;break;}
      if(free){free->fd=fd;free->address=address.sin_addr.s_addr;free->opened=millis();fcntl(fd,F_SETFL,O_NONBLOCK);int yes=1;setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes));}
      else ::close(fd);
    }
    Out* out=nullptr;
    for(unsigned i=0;i<8&&xQueueReceive(net->outgoing,&out,0)==pdTRUE;++i){
      Peer* target=nullptr;for(auto& p:net->peers)if(p.fd>=0&&same(p.token,out->token)){target=&p;break;}
      if(target&&target->count<32&&!target->closeCode){target->outgoing[(target->head+target->count)%32]=out;++target->count;}else freeOut(out);
    }
    for(auto& p:net->peers){
      SocketControl control{};lock();net->slots.takeControl(p.token,control);unlock();
      if(control.code&&!p.closeCode){p.closeCode=control.code;p.closeAt=millis();}
      if(control.release&&p.fd<0&&p.token.valid()){lock();net->slots.release(p.token);unlock();delete p.decoder;free(p.input);resetPeer(p);continue;}
      if(p.closedPending&&event(NetworkEventKind::Closed,p))p.closedPending=false;
      if(p.fd<0)continue;
      lock();bool stalled=net->slots.stalled(p.token,millis());unlock();
      if(stalled||(!p.web&&uint32_t(millis()-p.opened)>8000)||(p.web&&p.decoder->partial()&&uint32_t(millis()-p.frameStart)>8000)){drop(p);continue;}
      pump(p);if(p.fd<0||p.responseSize||p.closeCode)continue;
      uint8_t bytes[512];const int n=recv(p.fd,bytes,sizeof(bytes),MSG_DONTWAIT);
      if(n>0)received(p,bytes,n);else if(n==0||(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR))drop(p);
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  for(auto& p:net->peers){drop(p);delete p.decoder;free(p.input);resetPeer(p);}
  ::close(net->listener);net->listener=-1;
  Out* out=nullptr;while(xQueueReceive(net->outgoing,&out,0)==pdTRUE)freeOut(out);
  lock();net->done=true;unlock();vTaskDelete(nullptr);
}
}
bool startNetwork(const char* hostname,const char* ip,AssetLookup assets,SetupHandler setup){
  if(net)return false;
  net=new(std::nothrow) Network();if(!net)return false;
  net->mutex=xSemaphoreCreateMutex();net->events=xQueueCreate(16,sizeof(NetworkEvent));net->outgoing=xQueueCreate(64,sizeof(Out*));
  if(!net->mutex||!net->events||!net->outgoing){stopNetwork();return false;}
  snprintf(net->hostname,sizeof(net->hostname),"%s",hostname);snprintf(net->ip,sizeof(net->ip),"%s",ip);net->assets=assets;net->setup=setup;
  net->listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
  if(net->listener<0){stopNetwork();return false;}
  int yes=1;setsockopt(net->listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
  sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(11915);
  // Bind only the explicitly selected interface. The PC harness passes
  //127.0.0.1, so its test listener must never be exposed to the LAN.
  address.sin_addr.s_addr=inet_addr(ip);
  if(address.sin_addr.s_addr==INADDR_ANY||address.sin_addr.s_addr==INADDR_NONE){::close(net->listener);net->listener=-1;stopNetwork();return false;}
  if(bind(net->listener,(sockaddr*)&address,sizeof(address))||listen(net->listener,5)){::close(net->listener);net->listener=-1;stopNetwork();return false;}
  fcntl(net->listener,F_SETFL,O_NONBLOCK);
  // Setup parses bounded credential forms. Its deeper save chain gets measured
  // margin only while provisioning; ordinary Link keeps its original stack.
  if(xTaskCreate(task,"dspi-link-net",setup?SetupNetworkStack:LinkNetworkStack,nullptr,1,&net->task)!=pdPASS){stopNetwork();return false;}return true;
}
bool nextNetworkEvent(NetworkEvent& e){return net&&xQueueReceive(net->events,&e,0)==pdTRUE;}
void finishNetworkEvent(NetworkEvent& e){
  if(net&&(e.kind==NetworkEventKind::Text||e.kind==NetworkEventKind::Binary||e.kind==NetworkEventKind::Activity)){lock();net->slots.handled(e.token,e.length);unlock();}
  free(e.data);e.data=nullptr;
}
void releaseNetworkSession(SocketToken token){if(!net)return;lock();net->slots.requestRelease(token);unlock();}
bool sendNetwork(SocketToken token,uint8_t opcode,const uint8_t* data,size_t size){
  if(!net||size>LinkFrameLimit||(!data&&size))return false;
  lock();bool accepted=!net->stop&&net->slots.reserve(token,size,millis());unlock();if(!accepted)return false;
  auto* out=new(std::nothrow) Out();
  if(out){out->token=token;out->opcode=opcode;out->size=size;if(size)out->bytes=(uint8_t*)alloc(size);}
  if(!out||(size&&!out->bytes)){delete out;lock();net->slots.consumed(token,size,millis());unlock();return false;}
  if(size)memcpy(out->bytes,data,size);out->headerSize=ws_header_encode(opcode,size,out->header);
  if(xQueueSend(net->outgoing,&out,0)!=pdTRUE){freeOut(out);return false;}return true;
}
void closeNetwork(SocketToken token,uint16_t code){if(!net)return;lock();net->slots.requestClose(token,code);unlock();}
void setNetworkInfo(const char* info){if(!net)return;lock();snprintf(net->info,sizeof(net->info),"%s",info);unlock();}
void stopNetwork(){
  if(!net)return;
  if(net->mutex){lock();net->stop=true;unlock();}
  if(net->task&&!net->done)return;
  if(net->listener>=0)::close(net->listener);
  NetworkEvent e;if(net->events){while(xQueueReceive(net->events,&e,0)==pdTRUE)free(e.data);vQueueDelete(net->events);}
  if(net->outgoing)vQueueDelete(net->outgoing);if(net->mutex)vSemaphoreDelete(net->mutex);delete net;net=nullptr;
}
bool networkStopped(){if(!net)return true;lock();bool done=net->done;unlock();if(done)stopNetwork();return !net;}
}
