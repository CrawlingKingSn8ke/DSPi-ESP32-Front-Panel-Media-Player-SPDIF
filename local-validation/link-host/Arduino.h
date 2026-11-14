#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <new>
#include <string>
class String:public std::string {
 public:using std::string::string;String(const std::string& s):std::string(s){}String(unsigned value):std::string(std::to_string(value)){}
  void toCharArray(char* dst,size_t size)const{snprintf(dst,size,"%s",c_str());}
};
struct HostSerial{void println(const char*){}};inline HostSerial Serial;
struct HostEsp{unsigned getFreeHeap()const{return 200000;}unsigned getFreePsram()const{return 8000000;}};inline HostEsp ESP;
inline uint32_t hostClockOffset=0;
inline uint32_t millis(){static auto start=std::chrono::steady_clock::now();return hostClockOffset+uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count());}
using TaskHandle_t=std::thread*;
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
inline void vTaskDelay(unsigned ms){std::this_thread::sleep_for(std::chrono::milliseconds(ms));}
inline void vTaskDelete(void*){}
inline int xTaskCreate(void(*f)(void*),const char*,unsigned,void* arg,unsigned,TaskHandle_t* out){try{*out=new std::thread(f,arg);(*out)->detach();return 1;}catch(...){return 0;}}
