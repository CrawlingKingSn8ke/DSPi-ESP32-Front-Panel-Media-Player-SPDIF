#include "SmbSettings.h"
#include <cstring>
#include <cctype>
namespace SmbLibrary {
namespace {
template<size_t N>bool text(const char (&p)[N],bool empty=true){
 const auto* end=static_cast<const char*>(memchr(p,0,N));if(!end||(!empty&&end==p))return false;
 for(const unsigned char* q=(const unsigned char*)p;q<(const unsigned char*)end;){
  uint32_t c=*q++;if(c<32||c==127)return false;if(c<128)continue;
  unsigned n=c>=0xc2&&c<=0xdf?1:c>=0xe0&&c<=0xef?2:c>=0xf0&&c<=0xf4?3:0;
  if(!n||size_t(end-(const char*)q)<n)return false;
  uint32_t value=c&((1u<<(6-n))-1);for(unsigned i=0;i<n;++i){c=*q++;if((c&0xc0)!=0x80)return false;value=(value<<6)|(c&63);}
  if((n==1&&value<128)||(n==2&&value<2048)||(n==3&&value<65536)||value>0x10ffff||(value>=0xd800&&value<=0xdfff))return false;
 }
 return true;
}
uint32_t checksum(const uint8_t* p,size_t n){uint32_t value=2166136261u;for(size_t i=0;i<n;++i)value=(value^p[i])*16777619u;return value;}
bool valid(const Settings& s){
 if(!s.nextId)return false;
 for(unsigned i=0;i<kShares;++i)if(s.shares[i].id){
  if(s.shares[i].id>=s.nextId||!s.shares[i].revision||!validShare(s.shares[i]))return false;
  for(unsigned j=0;j<i;++j)if(s.shares[j].id==s.shares[i].id)return false;
 }return true;
}
struct Writer {
 uint8_t* p;size_t capacity,at=0;bool okay=true;
 void byte(uint8_t v){if(at>=capacity){okay=false;return;}p[at++]=v;}
 void word(uint32_t v){for(unsigned i=0;i<4;++i)byte(uint8_t(v>>(i*8)));}
 void string(const char* s){size_t n=strlen(s);byte(uint8_t(n));byte(uint8_t(n>>8));for(size_t i=0;i<n;++i)byte(uint8_t(s[i]));}
};
struct Reader {
 const uint8_t* p;size_t length,at=0;bool okay=true;
 uint8_t byte(){if(at>=length){okay=false;return 0;}return p[at++];}
 uint32_t word(){uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(byte())<<(i*8);return v;}
 template<size_t N>void string(char (&s)[N]){size_t n=byte();n|=size_t(byte())<<8;if(!okay||n>=N||n>length-at){okay=false;return;}memcpy(s,p+at,n);s[n]=0;if(memchr(s,0,n))okay=false;at+=n;}
};
}
bool validShare(const Share& s){
 const auto& c=s.connection;
 if(!text(s.label,false)||!text(s.root)||!text(c.server,false)||!text(c.share,false)||!text(c.user)||!text(c.domain)||!text(c.password))return false;
 for(const unsigned char* p=(const unsigned char*)c.server;*p;++p)if(!(std::isalnum(*p)||strchr(".-_:[]",*p)))return false;
 char check[kPath+1];return join("",c.share,check)&&join(s.root,"probe",check);
}
bool replaceShare(Settings& s,unsigned slot,const Share& candidate,bool keepPassword){
 if(slot>=kShares||!validShare(candidate)||!valid(s))return false;
 auto replacement=candidate;auto& old=s.shares[slot];
 if(keepPassword&&!candidate.connection.password[0]&&old.id){
  const auto& a=old.connection;const auto& b=candidate.connection;
  if(strcmp(a.server,b.server)||strcmp(a.share,b.share)||strcmp(a.user,b.user)||strcmp(a.domain,b.domain))return false;
  strcpy(replacement.connection.password,a.password);
 }
 if(old.id){if(old.revision==UINT32_MAX)return false;replacement.id=old.id;replacement.revision=old.revision+1;}
 else {if(s.nextId==UINT32_MAX)return false;replacement.id=s.nextId++;replacement.revision=1;}
 s.shares[slot]=replacement;return true;
}
bool removeShare(Settings& s,unsigned slot){if(slot>=kShares||!valid(s)||!s.shares[slot].id)return false;s.shares[slot]=Share{};return true;}
size_t encodeSettings(const Settings& s,uint8_t* bytes,size_t capacity){
 if(!bytes||capacity>kSettingsBytes||!valid(s))return 0;
 Writer out{bytes,capacity};out.word(0x43424d53);out.byte(1);out.word(s.sequence);out.word(s.nextId);out.byte(kShares);
 for(const auto& entry:s.shares){out.word(entry.id);if(!entry.id)continue;out.word(entry.revision);
  out.string(entry.label);out.string(entry.root);out.string(entry.connection.server);out.string(entry.connection.share);
  out.string(entry.connection.user);out.string(entry.connection.password);out.string(entry.connection.domain);
 }
 if(!out.okay||capacity-out.at<4)return 0;const auto hash=checksum(bytes,out.at);out.word(hash);return out.okay?out.at:0;
}
bool decodeSettings(const uint8_t* bytes,size_t length,Settings& result){
 if(!bytes||length<18||length>kSettingsBytes)return false;
 Reader tail{bytes+length-4,4};if(tail.word()!=checksum(bytes,length-4))return false;
 Reader in{bytes,length-4};Settings parsed;
 if(in.word()!=0x43424d53||in.byte()!=1)return false;parsed.sequence=in.word();parsed.nextId=in.word();if(in.byte()!=kShares)return false;
 for(auto& entry:parsed.shares){entry.id=in.word();if(!entry.id)continue;entry.revision=in.word();
  in.string(entry.label);in.string(entry.root);in.string(entry.connection.server);in.string(entry.connection.share);
  in.string(entry.connection.user);in.string(entry.connection.password);in.string(entry.connection.domain);
  if(!in.okay)return false;
 }
 if(!in.okay||in.at!=length-4||!valid(parsed))return false;result=parsed;return true;
}
}
