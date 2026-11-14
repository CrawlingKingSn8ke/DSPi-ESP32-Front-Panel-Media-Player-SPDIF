#pragma once
#include "Mp3ArtworkPolicy.h"
#include <algorithm>
#include <cstring>
namespace NetworkArtwork {
constexpr uint32_t MaximumJpeg=2*1024*1024,MaximumMetadata=8*1024*1024;
struct Location {uint64_t offset=0;uint32_t length=0;};
inline uint32_t be(const uint8_t* b){return uint32_t(b[0])<<24|uint32_t(b[1])<<16|uint32_t(b[2])<<8|b[3];}
template<class Read>bool jpeg(const Read& read,uint64_t at,uint64_t length,Location& out){
 uint8_t b[3];if(length<3||length>MaximumJpeg||!read(at,b,3)||b[0]!=255||b[1]!=216||b[2]!=255)return false;
 out={at,uint32_t(length)};return true;
}
template<class Read>bool flac(const Read& read,uint64_t size,uint64_t base,Location& out){
 uint8_t b[4];if(base>size||size-base<4||!read(base,b,4)||memcmp(b,"fLaC",4))return false;
 uint64_t at=base+4;Location fallback{};
 for(unsigned i=0;i<64&&at<=size&&size-at>=4;++i){
  if(!read(at,b,4))return false;const bool last=b[0]&128;const unsigned type=b[0]&127;
  const uint32_t count=uint32_t(b[1])<<16|uint32_t(b[2])<<8|b[3];at+=4;
  if(count>size-at||at-base+count>MaximumMetadata)return false;const uint64_t end=at+count;
  if(type==6&&count>=32){
   uint8_t n[8];if(!read(at,n,8))return false;const uint32_t kind=be(n),mime=be(n+4);uint64_t p=at+8;
   if(mime<=end-p&&end-p-mime>=4){p+=mime;if(!read(p,b,4))return false;const auto description=be(b);p+=4;
    if(description<=end-p&&end-p-description>=20){p+=description+16;if(!read(p,b,4))return false;const auto bytes=be(b);p+=4;Location candidate;
     if(bytes<=end-p&&jpeg(read,p,bytes,candidate)){if(kind==3){out=candidate;return true;}if(!fallback.length)fallback=candidate;}
    }
   }
  }
  at=end;if(last)break;
 }
 if(fallback.length){out=fallback;return true;}return false;
}
template<class Read>bool apic(const Read& read,uint64_t start,uint32_t size,bool v22,Location& out,uint8_t& kind){
 if(size<(v22?6u:5u))return false;const uint64_t end=start+size;uint8_t encoding;
 if(!read(start,&encoding,1)||encoding>3)return false;uint64_t p=start+1;
 if(v22)p+=3;
 else {uint8_t mime[64];const size_t n=std::min<uint64_t>(64,end-p);if(!n||!read(p,mime,n))return false;size_t i=0;while(i<n&&mime[i])++i;if(i==n)return false;p+=i+1;}
 if(p>=end||!read(p,&kind,1))return false;++p;if(p>=end)return false;
 uint8_t description[1024];const size_t n=std::min<uint64_t>(sizeof description,end-p);if(!read(p,description,n))return false;
 const unsigned step=(encoding==1||encoding==2)?2:1;
 for(size_t i=0;i+step<=n;i+=step)if(!description[i]&&(step==1||!description[i+1])){p+=i+step;return p<=end&&jpeg(read,p,end-p,out);}
 return false;
}
template<class Read>bool id3(const Read& read,const uint8_t* h,uint64_t size,Location& out,uint64_t& after){
 const auto tag=Mp3ArtworkPolicy::inspectId3TagHeader(h,10,size,MaximumMetadata);
 if(tag.status!=Mp3ArtworkPolicy::Id3TagStatus::Valid)return false;after=tag.tagEnd;
 const unsigned version=tag.version;uint64_t at=10;uint8_t b[10];Location fallback{};
 if(version>=3&&(h[5]&64)){
  if(tag.tagEnd-at<4||!read(at,b,4))return false;
  if(version==4&&((b[0]|b[1]|b[2]|b[3])&128))return false;
  const uint64_t skip=version==4?Mp3ArtworkPolicy::readSynchsafe32(b):4ULL+be(b);
  if(skip<4||skip>tag.tagEnd-at)return false;at+=skip;
 }
 for(unsigned i=0;i<512&&at<tag.tagEnd;++i){
  const unsigned header=version==2?6:10;if(tag.tagEnd-at<header||!read(at,b,header))return false;if(!b[0])break;
  const bool picture=version==2?!memcmp(b,"PIC",3):!memcmp(b,"APIC",4);
  if(version==4&&((b[4]|b[5]|b[6]|b[7])&128))return false;
  const uint32_t count=version==2?(uint32_t(b[3])<<16|uint32_t(b[4])<<8|b[5]):version==4?Mp3ArtworkPolicy::readSynchsafe32(b+4):be(b+4);
  at+=header;if(!count||count>tag.tagEnd-at)return false;
  const bool transformed=version==3?(b[9]&0xe0):version==4?(b[9]&0x4f):false;
  if(picture&&!transformed){Location candidate;uint8_t kind=255;if(apic(read,at,count,version==2,candidate,kind)){if(kind==3){out=candidate;return true;}if(!fallback.length)fallback=candidate;}}
  at+=count;
 }
 if(fallback.length){out=fallback;return true;}return false;
}
template<class Read>bool locate(const Read& read,uint64_t size,Location& out){
 out={};uint8_t h[10]{};if(size<4||!read(0,h,std::min<uint64_t>(10,size)))return false;
 if(!memcmp(h,"fLaC",4))return flac(read,size,0,out);
 if(size>=10&&!memcmp(h,"ID3",3)){uint64_t after=0;if(id3(read,h,size,out,after))return true;if(after)return flac(read,size,after,out);}
 return false;
}
}
