#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>

struct MediaReadResult {
  enum class Status : uint8_t { Data, Waiting, End, Error };
  Status status = Status::Waiting;
  size_t count = 0;
};

// Decoder-worker interface. Waiting is explicitly not EOF. Cancellation may
// be requested by loopTask and must never wait for network or SPI work.
class MediaByteSource {
public:
  virtual ~MediaByteSource() = default;
  virtual MediaReadResult read(uint8_t*,size_t) = 0;
  virtual bool seek(uint64_t) = 0;
  virtual uint64_t tell() const = 0;
  virtual bool length(uint64_t&) const = 0;
  virtual bool seekable() const = 0;
  virtual void cancel() = 0;
};

// Bounded storage; the platform adapter supplies synchronization. No malloc,
// filesystem, clocks or network operations occur while manipulating the FIFO.
class ByteFifo {
public:
  ByteFifo(uint8_t* storage,size_t capacity):bytes_(storage),capacity_(capacity){}
  size_t write(const uint8_t* input,size_t n) {
    if(done_ || cancelled_ || !input || !capacity_)return 0;
    n=std::min(n,capacity_-used_);
    const size_t first=std::min(n,capacity_-write_);
    memcpy(bytes_+write_,input,first); memcpy(bytes_,input+first,n-first);
    write_=(write_+n)%capacity_; used_+=n; return n;
  }
  MediaReadResult read(uint8_t* output,size_t n) {
    using S=MediaReadResult::Status;
    if(cancelled_)return {S::Error,0};
    n=std::min(n,used_);
    if(n && output){
      const size_t first=std::min(n,capacity_-read_);
      memcpy(output,bytes_+read_,first); memcpy(output+first,bytes_,n-first);
      read_=(read_+n)%capacity_; used_-=n; return {S::Data,n};
    }
    return {done_?(error_?S::Error:S::End):S::Waiting,0};
  }
  size_t available()const{return used_;}
  size_t writable()const{return capacity_-used_;}
  void finish(bool error){done_=true;error_=error;}
  void cancel(){cancelled_=true;}
  void reset(){read_=write_=used_=0;done_=error_=cancelled_=false;}
private:
  uint8_t* bytes_; size_t capacity_,read_=0,write_=0,used_=0;
  bool done_=false,error_=false,cancelled_=false;
};
