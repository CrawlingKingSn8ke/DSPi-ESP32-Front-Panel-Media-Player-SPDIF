#pragma once
#include "SmbSettings.h"
#include <atomic>
namespace SmbLibrary {
class Artwork {
 public:
  ~Artwork();
  bool start(const Share&,const char* folder,uint32_t generation,const char* track="");
  void cancel();bool stopped()const;uint16_t* take(uint32_t& generation);
 private:struct Task;Task* task_=nullptr;static void run(void*);
};
}
