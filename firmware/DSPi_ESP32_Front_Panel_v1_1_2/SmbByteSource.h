#pragma once
#include "MediaByteSource.h"
#include "SmbFileReader.h"
#include <atomic>
namespace SmbLibrary {
// Decoder-worker-only read/seek. Network waits never run on loopTask or TX.
class ByteSource:public MediaByteSource {
public:
 ByteSource(FileReader& reader,std::atomic<bool>& cancel,uint8_t* cache,size_t capacity);
 void reset(uint64_t length);
 MediaReadResult read(uint8_t*,size_t)override;
 bool seek(uint64_t)override;
 uint64_t tell()const override{return cursor_;}
 bool length(uint64_t& value)const override{value=length_;return true;}
 bool seekable()const override{return !cancel_&&!failed_;}
 void cancel()override{cancel_=true;}
private:
 FileReader& reader_;std::atomic<bool>& cancel_;uint8_t* cache_;size_t capacity_,used_=0;
 uint64_t length_=0,cursor_=0,start_=0;bool failed_=false;
};
}
