#pragma once
#include "DspiLinkWebSocket.h"
namespace DspiLink {
constexpr size_t SetupBodyLimit=2048;
constexpr uint32_t SetupWindowMs=600000;
class SetupWindow {
 public:
  void open(uint32_t now){opened_=now;open_=true;}
  void close(){open_=false;}
  bool active(uint32_t now)const{return open_&&uint32_t(now-opened_)<SetupWindowMs;}
  uint32_t remaining(uint32_t now)const{return active(now)?SetupWindowMs-uint32_t(now-opened_):0;}
 private:uint32_t opened_=0;bool open_=false;
};
// Setup is a different HTTP-only mode, entered physically. Never accept
// cookies, cross-origin forms, chunking or an unbounded request allocation.
inline unsigned validateSetup(const char* text,const char* ip,const char* hostname,size_t& bodyLength){
  bodyLength=0;size_t length=strnlen(text,LinkHttpLimit+1);
  if(length>LinkHttpLimit)return 431;
  if(length<4||memcmp(text+length-4,"\r\n\r\n",4))return 400;
  char copy[LinkHttpLimit+1];memcpy(copy,text,length+1);
  char* first=strstr(copy,"\r\n");if(!first)return 400;*first=0;
  if(strcmp(copy,"PUT /dspi/panel/network HTTP/1.1"))return 400;
  const char* names[]={"host","origin","content-type","content-length","transfer-encoding"};
  char* values[5]{};
  for(char* line=first+2;*line;){
    char* end=strstr(line,"\r\n");if(!end)return 400;*end=0;
    if(!*line){if(end+2!=copy+length)return 400;break;}
    char* colon=strchr(line,':');if(!colon||colon==line)return 400;
    for(char* p=line;p<colon;++p)if(!isalnum((unsigned char)*p)&&*p!='-')return 400;
    *colon=0;char* value=colon+1;while(*value==' '||*value=='\t')++value;
    for(char* p=value;*p;++p)if((unsigned char)*p<32&&*p!='\t')return 400;
    char* tail=value+strlen(value);while(tail>value&&(tail[-1]==' '||tail[-1]=='\t'))*--tail=0;
    for(unsigned i=0;i<5;++i)if(equalAscii(line,names[i])){if(values[i])return 400;values[i]=value;}
    line=end+2;
  }
  if(!values[0]||!*values[0]||!values[3]||!*values[3]||values[4])return 400;
  if(!values[1]||!origin_is_own(values[1],ip,hostname))return 403;
  if(!values[2]||!equalAscii(values[2],"application/json"))return 415;
  for(const char* p=values[3];*p;++p){if(*p<'0'||*p>'9')return 400;if(bodyLength>SetupBodyLimit)return 413;bodyLength=bodyLength*10+(*p-'0');}
  return !bodyLength?400:bodyLength>SetupBodyLimit?413:200;
}
}
