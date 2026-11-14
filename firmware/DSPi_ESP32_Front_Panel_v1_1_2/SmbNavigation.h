#pragma once
#include "SmbLibraryModel.h"
#include <cstring>
#include <cstdlib>
#include <climits>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif
namespace SmbLibrary {
// Only visited page cursors are retained, lazily in PSRAM. Entries stay paged.
class Navigation {
 public:
  Navigation()=default;~Navigation(){free(cursors_);}
  Navigation(const Navigation&)=delete;Navigation& operator=(const Navigation&)=delete;
  bool reset(const char* root){char test[kPath+1];if(!join(root,"probe",test)||!reserve(1))return false;strcpy(path_,root);depth_=0;cursor_=0;cursors_[0][0]=0;frames_[0]={};return true;}
  const char* path()const{return path_;}const char* after()const{return cursors_?cursors_[cursor_]:"";}
  bool hasPrevious()const{return cursor_>frames_[depth_].base;}
  unsigned pageIndex()const{return cursor_-frames_[depth_].base;}
  bool previous(){if(!hasPrevious())return false;--cursor_;return true;}
  bool next(const char* after){if(cursor_>UINT_MAX-2||!after||strlen(after)>kName||!reserve(cursor_+2))return false;strcpy(cursors_[++cursor_],after);return true;}
  bool push(const char* name,unsigned row){char next[kPath+1];if(depth_+1>=32||cursor_>UINT_MAX-2||!join(path_,name,next)||!reserve(cursor_+2))return false;frames_[depth_].row=row;frames_[depth_].length=strlen(path_);frames_[depth_].cursor=cursor_;strcpy(path_,next);++depth_;frames_[depth_]={};frames_[depth_].base=++cursor_;cursors_[cursor_][0]=0;return true;}
  bool pop(unsigned& row){if(!depth_)return false;--depth_;auto& f=frames_[depth_];path_[f.length]=0;cursor_=f.cursor;row=f.row;return true;}
 private:
  bool reserve(unsigned count){
   if(count<=capacity_)return true;
   if(count>UINT_MAX-15)return false;const unsigned capacity=(count+15)&~15u;
   const size_t bytes=size_t(capacity)*sizeof(Cursor);if(bytes/sizeof(Cursor)!=capacity)return false;
#ifdef ESP_PLATFORM
   void* p=heap_caps_realloc(cursors_,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
   void* p=realloc(cursors_,bytes);
#endif
   if(!p)return false;cursors_=static_cast<Cursor*>(p);capacity_=capacity;return true;
  }
  using Cursor=char[kName+1];
  struct Frame{unsigned length=0,cursor=0,base=0,row=0;};Frame frames_[32];char path_[kPath+1]={0};Cursor* cursors_=nullptr;unsigned capacity_=0,depth_=0,cursor_=0;
};
}
