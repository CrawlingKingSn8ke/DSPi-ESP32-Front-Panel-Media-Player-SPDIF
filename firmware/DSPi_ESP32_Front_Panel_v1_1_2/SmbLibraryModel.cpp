#include "SmbLibraryModel.h"
#include <cstring>
#include <algorithm>
#include <cctype>
namespace SmbLibrary {
namespace {
bool component(const char* p,size_t n){
 if(!p||!n||n>kName||(n==1&&p[0]=='.')||(n==2&&p[0]=='.'&&p[1]=='.')||p[n-1]==' '||p[n-1]=='.')return false;
 for(size_t i=0;i<n;++i){unsigned char c=p[i];if(c<32||c==127||strchr("/\\:*?\"<>|",c))return false;}
 return true;
}
bool relative(const char* p){
 if(!p||strlen(p)>kPath)return false;
 while(*p){const char* end=strchr(p,'/');const size_t n=end?size_t(end-p):strlen(p);if(!component(p,n))return false;if(!end)return true;p=end+1;if(!*p)return false;}
 return true;
}
}
bool join(const char* parent,const char* name,char (&out)[kPath+1]){
 out[0]=0;
 if(!relative(parent)||!name||!component(name,strlen(name)))return false;
 const size_t a=strlen(parent),b=strlen(name);if(a+b+(a?1:0)>kPath)return false;
 memcpy(out,parent,a);size_t at=a;if(a)out[at++]='/';memcpy(out+at,name,b+1);return true;
}
bool playable(const char* name){
 if(!name)return false;const char* ext=strrchr(name,'.');if(!ext)return false;
 char lower[6]={0};if(strlen(ext)>=sizeof(lower))return false;
 for(size_t i=0;ext[i];++i)lower[i]=char(std::tolower(static_cast<unsigned char>(ext[i])));
 return !strcmp(lower,".wav")||!strcmp(lower,".flac")||!strcmp(lower,".mp3");
}
void Page::begin(const char* cursor){
 count=0;total=0;more=false;after[0]=0;
 if(cursor&&strlen(cursor)<=kName)memcpy(after,cursor,strlen(cursor)+1);
}
bool Page::offer(const char* name,bool directory,uint64_t size,uint64_t modified){
 if(!name||!component(name,strlen(name)))return false;
 if(!directory&&!playable(name))return true;
 if(total<UINT32_MAX)++total;
 if(strcmp(name,after)<=0)return true;
 size_t at=0;while(at<count&&strcmp(entries[at].name,name)<0)++at;
 if(at<count&&!strcmp(entries[at].name,name))return true;
 if(count==kPage){more=true;if(at==kPage)return true;}else ++count;
 for(size_t i=count-1;i>at;--i)entries[i]=entries[i-1];
 auto& e=entries[at];memcpy(e.name,name,strlen(name)+1);e.directory=directory;e.size=size;e.modified=modified;return true;
}
}
