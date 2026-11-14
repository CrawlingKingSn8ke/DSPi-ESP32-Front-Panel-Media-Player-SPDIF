#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
extern "C" {
#include "src/dspi_link_vendor/origin.h"
}

namespace DspiLink {
constexpr size_t LinkHttpLimit=2048, LinkFrameLimit=16384;
struct Upgrade { char key[25]{}; };
inline bool equalAscii(const char* a,const char* b) {
  while(*a&&*b) if(tolower((unsigned char)*a++)!=tolower((unsigned char)*b++))return false;
  return *a==*b;
}
inline bool hasToken(const char* list,const char* token) {
  while(*list) {
    while(*list==' '||*list=='\t'||*list==',')++list;
    const char* end=strchr(list,',');if(!end)end=list+strlen(list);
    const char* trimmed=end;while(trimmed>list&&(trimmed[-1]==' '||trimmed[-1]=='\t'))--trimmed;
    if(size_t(trimmed-list)==strlen(token)) {
      size_t i=0;for(;list+i<trimmed;++i)if(tolower((unsigned char)list[i])!=tolower((unsigned char)token[i]))break;
      if(list+i==trimmed)return true;
    }
    list=*end?end+1:end;
  }
  return false;
}
inline bool validKey(const char* key) {
  static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if(strlen(key)!=24||key[22]!='='||key[23]!='=')return false;
  for(unsigned i=0;i<22;++i)if(!strchr(alphabet,key[i]))return false;
  return (size_t(strchr(alphabet,key[21])-alphabet)&15)==0;
}

// No socket state is allocated and no101 is sent until this succeeds.
// Host is not an authority for Origin: only the actual local IP/hostname is.
inline unsigned validateUpgrade(const char* text,const char* ip,const char* hostname,bool room,Upgrade& result) {
  const size_t length=strnlen(text,LinkHttpLimit+1);
  if(length>LinkHttpLimit)return 431;
  if(length<4||memcmp(text+length-4,"\r\n\r\n",4))return 400;
  char copy[LinkHttpLimit+1];memcpy(copy,text,length+1);
  char* first=strstr(copy,"\r\n");if(!first)return 400;*first=0;
  if(strcmp(copy,"GET /dspi/v1 HTTP/1.1"))return 400;
  const char* names[]={"host","upgrade","connection","sec-websocket-key","sec-websocket-version","sec-websocket-protocol","origin","content-length","transfer-encoding"};
  char* values[9]{};
  for(char* line=first+2;*line;) {
    char* end=strstr(line,"\r\n");if(!end)return 400;*end=0;
    if(!*line){if(end+2!=copy+length)return 400;break;}
    char* colon=strchr(line,':');if(!colon||colon==line)return 400;
    for(char* p=line;p<colon;++p)if(!isalnum((unsigned char)*p)&&*p!='-')return 400;
    *colon=0;char* value=colon+1;while(*value==' '||*value=='\t')++value;
    for(char* p=value;*p;++p)if((unsigned char)*p<32&&*p!='\t')return 400;
    char* tail=value+strlen(value);while(tail>value&&(tail[-1]==' '||tail[-1]=='\t'))*--tail=0;
    for(unsigned i=0;i<9;++i)if(equalAscii(line,names[i])){if(values[i])return 400;values[i]=value;}
    line=end+2;
  }
  if(!values[0]||!*values[0]||!values[1]||!equalAscii(values[1],"websocket")||
     !values[2]||!hasToken(values[2],"upgrade")||!values[3]||!validKey(values[3])||
     !values[4]||strcmp(values[4],"13")||!values[5]||!hasToken(values[5],"dspi-link-1")||values[7]||values[8])return 400;
  if(values[6]&&!origin_is_own(values[6],ip,hostname))return 403;
  if(!room)return 503;
  memcpy(result.key,values[3],25);return 101;
}

inline bool validUtf8(const uint8_t* p,size_t size) {
  for(size_t i=0;i<size;) {
    uint32_t cp=p[i++];unsigned more=0;uint32_t minimum=0;
    if(cp<0x80)continue;
    if(cp>=0xc2&&cp<=0xdf){more=1;cp&=31;minimum=0x80;}
    else if(cp>=0xe0&&cp<=0xef){more=2;cp&=15;minimum=0x800;}
    else if(cp>=0xf0&&cp<=0xf4){more=3;cp&=7;minimum=0x10000;}else return false;
    if(more>size-i)return false;
    while(more--){if((p[i]&0xc0)!=0x80)return false;cp=(cp<<6)|(p[i++]&63);}
    if(cp<minimum||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return false;
  }
  return true;
}
inline bool jsonDepthAllowed(const char* text) {
  unsigned depth=0;bool quoted=false,escape=false;
  for(const unsigned char* p=(const unsigned char*)text;*p;++p) {
    if(quoted){if(escape)escape=false;else if(*p=='\\')escape=true;else if(*p=='"')quoted=false;}
    else if(*p=='"')quoted=true;
    else if(*p=='{'||*p=='['){if(++depth>8)return false;}
    else if(*p=='}'||*p==']'){if(!depth)return false;--depth;}
  }
  return depth==0&&!quoted;
}

enum class WsEvent : uint8_t { More, Frame, Error };
class WsDecoder {
 public:
  WsDecoder(uint8_t* payload,size_t capacity):data_(payload),capacity_(capacity){}
  void reset(){headerCount_=0;needed_=2;length_=0;offset_=0;error_=0;done_=false;}
  uint8_t opcode()const{return header_[0]&15;}
  size_t length()const{return length_;}
  uint16_t error()const{return error_;}
  bool partial()const{return headerCount_!=0;}
  WsEvent feed(uint8_t byte) {
    if(error_)return WsEvent::Error;
    if(done_)return fail(1002);
    if(headerCount_<needed_) {
      header_[headerCount_++]=byte;
      if(headerCount_==2) {
        const uint8_t op=opcode(),shortLen=header_[1]&127;
        if((header_[0]&0xf0)!=0x80||!(header_[1]&0x80)||
           (op!=1&&op!=2&&op!=8&&op!=9&&op!=10)||(op>=8&&shortLen>125))return fail(1002);
        needed_=2+(shortLen==126?2:shortLen==127?8:0)+4;
      }
      // Reject impossible lengths before reading mask or allocating memory.
      const unsigned ext=(header_[1]&127)==126?2:(header_[1]&127)==127?8:0;
      if(headerCount_==2+ext) {
        uint64_t n=header_[1]&127;
        if(ext){n=0;for(unsigned i=0;i<ext;++i)n=(n<<8)|header_[2+i];}
        if((ext==2&&n<126)||(ext==8&&n<=65535))return fail(1002);
        if(n>capacity_||n>LinkFrameLimit)return fail(1009);
        length_=size_t(n);
      }
      if(headerCount_<needed_)return WsEvent::More;
      if(length_==0)return complete();
      return WsEvent::More;
    }
    data_[offset_]=byte^header_[needed_-4+(offset_&3)];++offset_;
    return offset_==length_?complete():WsEvent::More;
  }
 private:
  WsEvent fail(uint16_t code){error_=code;return WsEvent::Error;}
  WsEvent complete(){
    if(opcode()==1&&!validUtf8(data_,length_))return fail(1007);
    if(opcode()==8){
      if(length_==1)return fail(1002);
      if(length_>=2){const unsigned code=(unsigned(data_[0])<<8)|data_[1];
        if(code<1000||code>=5000||code==1004||code==1005||code==1006||code==1015||(code>1014&&code<3000))return fail(1002);
        if(!validUtf8(data_+2,length_-2))return fail(1007);
      }
    }
    done_=true;return WsEvent::Frame;
  }
  uint8_t* data_;size_t capacity_,length_=0,offset_=0;
  uint8_t header_[14]{};unsigned headerCount_=0,needed_=2;
  uint16_t error_=0;bool done_=false;
};
}
