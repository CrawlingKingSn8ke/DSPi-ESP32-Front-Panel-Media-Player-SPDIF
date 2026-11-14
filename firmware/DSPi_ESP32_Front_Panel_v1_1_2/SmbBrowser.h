#pragma once
#include "SmbSettings.h"
namespace SmbLibrary {
struct BrowseResponse {
 uint32_t generation=0,share=0,revision=0;bool okay=false,cancelled=false;
 char path[kPath+1]={0},after[kName+1]={0},error[96]={0};Page page;
};
class Browser {
public:
 ~Browser();bool browse(const Share&,const char* path,const char* after,uint32_t generation);
 void cancel();bool busy()const;bool stopped()const;BrowseResponse* take();
 static void freeResponse(BrowseResponse*);
private:
 struct Task;Task* task_=nullptr;static void run(void*);
};
}
