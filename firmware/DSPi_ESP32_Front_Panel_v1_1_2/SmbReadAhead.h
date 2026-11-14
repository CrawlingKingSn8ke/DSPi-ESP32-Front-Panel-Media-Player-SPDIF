#pragma once
#include "SmbFileReader.h"
#include <atomic>
#include <mutex>
namespace SmbLibrary {
// A separate permanent worker is the only caller of upstream.readAt while
// started. Decoder reads committed bytes; no metadata lock spans network I/O.
class ReadAhead:public FileReader {
public:
 ReadAhead(FileReader& upstream,const std::atomic<bool>& cancel,uint8_t* bytes,size_t capacity);
 ~ReadAhead();
 ReadAhead(const ReadAhead&)=delete;ReadAhead& operator=(const ReadAhead&)=delete;
 bool start(uint64_t length);
 void stop();
 int readAt(uint64_t offset,uint8_t* bytes,size_t count)override;
 uint64_t demandMicroseconds()const{return demandUs_.load();}
 size_t bufferedBytes(){std::lock_guard<std::mutex> lock(mutex_);return used_;}
 size_t capacityBytes()const{return capacity_;}
private:
 static void run(void*);
 FileReader& upstream_;const std::atomic<bool>& cancel_;uint8_t* bytes_;size_t capacity_;
 std::mutex mutex_;uint64_t length_=0,start_=0,generation_=0;
 size_t head_=0,used_=0;bool failed_=false,started_=false;
 std::atomic<bool> stopping_{false};std::atomic<uint64_t> demandUs_{0};
};
}
