#include "SmbByteSource.h"
namespace SmbLibrary {
ByteSource::ByteSource(FileReader& reader,std::atomic<bool>& cancel,uint8_t* cache,size_t capacity):
 reader_(reader),cancel_(cancel),cache_(cache),capacity_(std::min<size_t>(capacity,32768)){}
void ByteSource::reset(uint64_t length){length_=length;cursor_=start_=0;used_=0;failed_=false;}
MediaReadResult ByteSource::read(uint8_t* out,size_t wanted){
 using S=MediaReadResult::Status;
 if(cancel_||failed_||!cache_||!capacity_||(!out&&wanted))return {S::Error,0};
 if(!wanted)return {S::Waiting,0};
 if(cursor_==length_)return {S::End,0};
 if(cursor_<start_||cursor_-start_>=used_){
  const size_t request=size_t(std::min<uint64_t>(capacity_,length_-cursor_));
  const int count=reader_.readAt(cursor_,cache_,request);
  if(cancel_)return {S::Error,0};
  if(count<=0||size_t(count)>request){failed_=true;return {S::Error,0};}
  start_=cursor_;used_=size_t(count);
 }
 const size_t at=size_t(cursor_-start_),count=std::min(wanted,used_-at);
 memcpy(out,cache_+at,count);cursor_+=count;return {S::Data,count};
}
bool ByteSource::seek(uint64_t offset){if(cancel_||failed_||offset>length_)return false;cursor_=offset;return true;}
}
