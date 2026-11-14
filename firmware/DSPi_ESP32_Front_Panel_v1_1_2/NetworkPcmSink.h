#pragma once
#include <cstddef>
#include <cstdint>
class NetworkPcmSink {
public:
 virtual ~NetworkPcmSink()=default;
 virtual size_t write(uint32_t token,const int32_t*,size_t frames)=0;
 virtual void finish(uint32_t token,bool failed)=0;
};
