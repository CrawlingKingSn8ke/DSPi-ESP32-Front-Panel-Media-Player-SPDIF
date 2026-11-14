#pragma once
#include "PanelControlMenus.h"

// No transport/task ownership: one bounded operation through the panel's UART.
class SourceNames {
 public:
  enum class Support:uint8_t {Unknown,Legacy,Native};
  enum class Result:uint8_t {None,Confirmed,Changed,Unknown};
  using Exchange=PanelControlMenus::Reply(*)(void*,const PanelControlMenus::Frame&);
  Support support()const{return support_;}
  bool known(uint8_t source)const{return source<7&&(known_&(1u<<source));}
  const char* name(uint8_t source)const{return known(source)?names_[source]:"";}
  bool busy()const{return stage_!=0;}
  Result takeResult(){Result r=result_;result_=Result::None;return r;}
  bool takeChanged(){bool value=changed_;changed_=false;return value;}
  void reset(){*this=SourceNames();}
  void cancel(){if(stage_<3){stage_=0;deadline_=0;}}
  bool begin(uint8_t source,const char* text,const char* expected=nullptr){
    if(support_!=Support::Native||!known(source)||busy()||!valid(text))return false;
    result_=Result::None;
    if(expected&&strcmp(expected,names_[source])){result_=Result::Changed;return false;}
    if(!strcmp(text,names_[source])){result_=Result::Confirmed;return true;}
    source_=source;strcpy(draft_,text);strcpy(original_,names_[source]);
    stage_=1;deadline_=0;nextAt_=0;return true;
  }
  bool notification(const uint8_t* p,uint16_t n){
    if(support_!=Support::Native||!p||n!=40||p[0]!=2||p[1]!=0x0f||p[2]||p[4]>=7||p[6]||p[7])return false;
    char text[32];if(!decode(p+8,text))return false;publish(p[4],text);return true;
  }
  bool service(uint32_t now,bool canWrite,Exchange exchange,void* context){
    using namespace PanelControlMenus;
    if(support_==Support::Legacy)return false;
    if(stage_&&!deadline_)deadline_=now+4000;
    if(stage_&&int32_t(now-deadline_)>=0){finish(Result::Unknown);return false;}
    if(nextAt_&&int32_t(now-nextAt_)<0)return false;
    if(stage_==2&&!canWrite)return false;
    const uint8_t source=stage_?source_:support_==Support::Unknown?0:cursor_;
    Frame f;f.get=stage_!=2;f.request=f.get?0x55:0x54;f.value=source;f.length=32;
    if(!f.get){
      if(strcmp(names_[source],original_)){finish(Result::Changed);return false;}
      f.length=strlen(draft_)+1;memcpy(f.bytes,draft_,f.length);
      stage_=3; // Mark sent before dispatch: a lost ACK must never replay SET.
    }
    Reply r=exchange(context,f);nextAt_=now+100;
    if(!f.get){if(r.status==WireStatus::Busy)stage_=2;return true;}
    if(r.status==WireStatus::Busy)return true;
    if(support_==Support::Unknown){
      if(r.status==WireStatus::Rejected||(r.status==WireStatus::Ok&&r.size==4)){support_=Support::Legacy;return true;}
      if(r.status!=WireStatus::Ok||r.size!=32){nextAt_=now+2000;return true;}
    }
    char text[32];
    if(r.status!=WireStatus::Ok||r.size!=32||!decode(r.bytes,text)){nextAt_=now+500;return true;}
    support_=Support::Native;publish(source,text);
    if(stage_==1){if(strcmp(text,original_))finish(Result::Changed);else stage_=2;}
    else if(stage_==3)finish(strcmp(text,draft_)?Result::Changed:Result::Confirmed);
    else {cursor_=(source+1)%7;nextAt_=now+500;}
    return true;
  }
 private:
  static bool valid(const char* s){if(!s)return false;unsigned i=0;for(;i<32&&s[i];++i)if(uint8_t(s[i])<32||uint8_t(s[i])>126)return false;return i<32;}
  static bool decode(const uint8_t* bytes,char* text){unsigned i=0;for(;i<32&&bytes[i];++i){if(bytes[i]<32||bytes[i]>126)return false;text[i]=bytes[i];}if(i==32)return false;text[i]=0;return true;}
  void publish(uint8_t source,const char* text){if(!known(source)||strcmp(names_[source],text)){strcpy(names_[source],text);changed_=true;}known_|=1u<<source;}
  void finish(Result result){stage_=0;deadline_=0;result_=result;}
  Support support_=Support::Unknown;Result result_=Result::None;
  char names_[7][32]{},draft_[32]{},original_[32]{};
  uint8_t known_=0,cursor_=0,stage_=0,source_=0;bool changed_=false;
  uint32_t nextAt_=0,deadline_=0;
};
