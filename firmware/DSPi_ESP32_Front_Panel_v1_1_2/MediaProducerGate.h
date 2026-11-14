#pragma once
#include <atomic>
#include <cstdint>

// Single network PCM producer, main-loop lifecycle owner. Invalidate before
// stopping output; reset/free the PCM ring only after quiescent(). A failed
// enter never owns the ring, even if it races a later open().
class MediaProducerGate {
public:
  uint32_t open(){
    if(!quiescent() || live_.load())return 0;
    if(++serial_==0)++serial_;
    live_.store(serial_); return serial_;
  }
  bool enter(uint32_t token){
    writers_.fetch_add(1);
    if(token && live_.load()==token)return true;
    writers_.fetch_sub(1); return false;
  }
  void leave(){writers_.fetch_sub(1);}
  void invalidate(){live_.store(0);}
  bool quiescent()const{return writers_.load()==0;}
  bool current(uint32_t token)const{return token && live_.load()==token;}
private:
  std::atomic<uint32_t> live_{0},writers_{0};
  uint32_t serial_=0;
};
