#pragma once
#include <cstddef>
#include <cstdint>
namespace SmbLibrary {
constexpr size_t kPath=1024,kName=255,kPage=16;
bool join(const char* parent,const char* name,char (&out)[kPath+1]);
bool playable(const char* name);
struct Entry{char name[kName+1]={0};bool directory=false;uint64_t size=0,modified=0;};
struct Page{
 Entry entries[kPage];size_t count=0;uint32_t total=0;bool more=false;char after[kName+1]={0};
 void begin(const char* cursor);
 bool offer(const char* name,bool directory,uint64_t size,uint64_t modified);
};
}
