#include "SmbClient.h"
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <fcntl.h>
#ifdef _WIN32
#include <winsock2.h>
#elif defined(ESP_PLATFORM)
#include <sys/poll.h>
#else
#include <poll.h>
#endif
extern "C" {
#ifdef SMB_HOST_PROBE
#include "compat.h"
#include "smb2/smb2.h"
#include "smb2/libsmb2.h"
#include "smb2/libsmb2-raw.h"
#include "smb2/smb2-errors.h"
#else
#include "src/smb_vendor/compat.h"
#include "src/smb_vendor/smb2.h"
#include "src/smb_vendor/libsmb2.h"
#include "src/smb_vendor/libsmb2-raw.h"
#include "src/smb_vendor/smb2-errors.h"
#endif
}
#ifdef close
#undef close
#endif
namespace SmbLibrary {
struct Client::Reply {
 bool done=false;int status=-EIO;void* value=nullptr;
 enum Kind{Generic,Create,Query} kind=Generic;
 smb2_file_id id{};Page* page=nullptr;
 static void callback(smb2_context* c,int status,void* data,void* arg){
  auto& r=*static_cast<Reply*>(arg);r.status=status;r.value=data;
  if(!status&&r.kind==Create&&data)memcpy(r.id,static_cast<smb2_create_reply*>(data)->file_id,SMB2_FD_SIZE);
  if(!status&&r.kind==Query){
   auto* reply=static_cast<smb2_query_directory_reply*>(data);
   if(!reply||!reply->output_buffer_length||reply->output_buffer_length>8192){r.status=-EIO;r.done=true;return;}
   uint32_t at=0;
   while(at<reply->output_buffer_length){
    smb2_iovec slice{};slice.buf=reply->output_buffer+at;slice.len=reply->output_buffer_length-at;
    smb2_fileidfulldirectoryinformation info{};
    if(smb2_decode_fileidfulldirectoryinformation(c,&info,&slice)){r.status=-EIO;break;}
    // Do not follow remote junctions/symlinks outside the configured share.
    if(!(info.file_attributes&SMB2_FILE_ATTRIBUTE_REPARSE_POINT))
     r.page->offer(info.name,info.file_attributes&SMB2_FILE_ATTRIBUTE_DIRECTORY,info.end_of_file,info.last_write_time.tv_sec);
    free(const_cast<char*>(info.name));
    if(!info.next_entry_offset)break;
    if(info.next_entry_offset<80||info.next_entry_offset>=slice.len){r.status=-EIO;break;}
    at+=info.next_entry_offset;
   }
  }
  r.done=true;
 }
};
Client::~Client(){close();}
void Client::close(){if(context_)smb2_destroy_context(context_);context_=nullptr;file_=nullptr;}
bool Client::failed(const char* text){strncpy(error_,text,sizeof(error_)-1);error_[sizeof(error_)-1]=0;return false;}
bool Client::wait(Reply& r){
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
 while(!r.done){
  if(cancel_||std::chrono::steady_clock::now()>=deadline){close();return failed(cancel_?"Cancelled":"Server timed out");}
  if(!context_)return failed("Server disconnected");
  const auto pollStarted=std::chrono::steady_clock::now();
#ifdef _WIN32
  WSAPOLLFD p{};p.fd=smb2_get_fd(context_);p.events=short(smb2_which_events(context_));int ready=WSAPoll(&p,1,100);
#else
  pollfd p{};p.fd=smb2_get_fd(context_);p.events=short(smb2_which_events(context_));int ready=poll(&p,1,100);
#endif
  waitPollUs_+=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-pollStarted).count());
  ++waitPollCalls_;if(!ready)++waitPollTimeouts_;
  if(ready<0){close();return failed("Server disconnected");}
  // Same event/idle servicing and100ms poll as before; only measure the
  // boundary. These are wall times, not CPU cycles or radio-only latency.
  const auto serviceStarted=std::chrono::steady_clock::now();
  const int serviced=smb2_service(context_,ready?p.revents:0);
  const auto serviceUs=uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-serviceStarted).count());
  waitServiceUs_+=serviceUs;currentServiceMaxUs_=std::max(currentServiceMaxUs_,serviceUs);
  if(serviced<0){close();return failed("Server disconnected");}
 }
 return true;
}
bool Client::connect(const Credentials& s){
 close();error_[0]=0;if(cancel_)return failed("Cancelled");
 if(!s.server[0]||!s.share[0]||!memchr(s.server,0,sizeof(s.server))||!memchr(s.share,0,sizeof(s.share))||
    !memchr(s.user,0,sizeof(s.user))||!memchr(s.password,0,sizeof(s.password))||!memchr(s.domain,0,sizeof(s.domain)))return failed("Invalid share settings");
 context_=smb2_init_context();if(!context_)return failed("SMB memory unavailable");
 smb2_set_timeout(context_,5);smb2_set_version(context_,SMB2_VERSION_ANY);
 smb2_set_authentication(context_,SMB2_SEC_NTLMSSP);
 smb2_set_security_mode(context_,SMB2_NEGOTIATE_SIGNING_ENABLED);
 smb2_set_user(context_,s.user);smb2_set_password(context_,s.password);
 if(s.domain[0])smb2_set_domain(context_,s.domain);
 Reply r;
 if(smb2_connect_share_async(context_,s.server,s.share,s.user,Reply::callback,&r)<0){close();return failed("Cannot connect to share");}
 if(!wait(r))return false;
 if(r.status){close();return failed("Share access denied or unavailable");}return true;
}
bool Client::list(const char* path,const char* after,Page& page){
 char checked[kPath+1];
 if(!context_||file_||!join(path,"probe",checked)||!after||strlen(after)>kName)return failed("Invalid folder");
 if(*after&&!join("",after,checked))return failed("Invalid folder cursor");
 page.begin(after);Reply created;created.kind=Reply::Create;
 smb2_create_request create{};create.impersonation_level=SMB2_IMPERSONATION_IMPERSONATION;
 create.desired_access=SMB2_FILE_LIST_DIRECTORY|SMB2_FILE_READ_ATTRIBUTES;
 create.file_attributes=SMB2_FILE_ATTRIBUTE_DIRECTORY;
 create.share_access=SMB2_FILE_SHARE_READ|SMB2_FILE_SHARE_WRITE|SMB2_FILE_SHARE_DELETE;
 create.create_disposition=SMB2_FILE_OPEN;create.create_options=SMB2_FILE_DIRECTORY_FILE;create.name=path;
 auto* request=smb2_cmd_create_async(context_,&create,Reply::callback,&created);
 if(!request)return failed("Cannot open folder");smb2_queue_pdu(context_,request);
 if(!wait(created))return false;
 if(created.status)return failed("Folder unavailable");
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
 bool okay=false;
 for(unsigned calls=0;calls<4096&&!cancel_;++calls){
  if(std::chrono::steady_clock::now()>=deadline){failed("Folder listing timed out");break;}
  Reply result;result.kind=Reply::Query;result.page=&page;
  smb2_query_directory_request query{};query.file_information_class=SMB2_FILE_ID_FULL_DIRECTORY_INFORMATION;
  memcpy(query.file_id,created.id,SMB2_FD_SIZE);query.output_buffer_length=8192;query.name="*";
  request=smb2_cmd_query_directory_async(context_,&query,Reply::callback,&result);
  if(!request){failed("Cannot read folder");break;}smb2_queue_pdu(context_,request);
  if(!wait(result))return false;
  if(uint32_t(result.status)==SMB2_STATUS_NO_MORE_FILES){okay=true;break;}
  if(result.status){failed("Cannot read folder");break;}
 }
 if(context_){
  Reply result;smb2_close_request closeRequest{};memcpy(closeRequest.file_id,created.id,SMB2_FD_SIZE);
  request=smb2_cmd_close_async(context_,&closeRequest,Reply::callback,&result);
  if(request){smb2_queue_pdu(context_,request);if(!wait(result)||result.status)okay=false;}
  else {close();okay=false;}
 }
 if(!okay&&!error_[0])failed(cancel_?"Cancelled":"Folder too large");return okay;
}
bool Client::open(const char* path,Identity& identity){
 char checked[kPath+1];if(!context_||file_||!path||!*path||!join(path,"probe",checked))return failed("Invalid audio path");
 Reply opened;if(smb2_open_async(context_,path,O_RDONLY,Reply::callback,&opened)<0)return failed("Cannot open file");
 if(!wait(opened))return false;if(opened.status)return failed("File unavailable");file_=static_cast<smb2fh*>(opened.value);
 smb2_stat_64 st{};Reply stat;
 if(smb2_fstat_async(context_,file_,&st,Reply::callback,&stat)<0||!wait(stat)||stat.status||st.smb2_type!=SMB2_TYPE_FILE){close();return failed("Cannot read file information");}
 identity={st.smb2_ino,st.smb2_size,st.smb2_mtime,st.smb2_mtime_nsec};return true;
}
int Client::readAt(uint64_t offset,uint8_t* bytes,size_t count){
 const auto started=std::chrono::steady_clock::now();
 const auto pollBefore=waitPollUs_,serviceBefore=waitServiceUs_;const auto callsBefore=waitPollCalls_,timeoutsBefore=waitPollTimeouts_;currentServiceMaxUs_=0;
 auto measured=[&](int result){
  const auto elapsed=uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count());
  ++readCalls_;readUs_+=elapsed;if(result>0)readBytes_+=result;
  auto max=readMaxUs_.load();while(max<elapsed&&!readMaxUs_.compare_exchange_weak(max,elapsed)){}
  readPollUs_+=waitPollUs_-pollBefore;readServiceUs_+=waitServiceUs_-serviceBefore;
  readPollCalls_+=waitPollCalls_-callsBefore;readPollTimeouts_+=waitPollTimeouts_-timeoutsBefore;
  auto serviceMax=readServiceMaxUs_.load();while(serviceMax<currentServiceMaxUs_&&!readServiceMaxUs_.compare_exchange_weak(serviceMax,currentServiceMaxUs_)){}
  if(result<0)++readErrors_;if(elapsed>=100000)++readOver100ms_;if(elapsed>=250000)++readOver250ms_;if(elapsed>=500000)++readOver500ms_;
  return result;
 };
 if(!context_||!file_||!bytes||!count||count>32768||cancel_){failed("Invalid or cancelled read");return -1;}
 Reply read;if(smb2_pread_async(context_,file_,bytes,uint32_t(count),offset,Reply::callback,&read)<0){failed("Cannot read file");return measured(-1);}
 if(!wait(read))return measured(-1);if(read.status<0||size_t(read.status)>count){failed("File read failed");return measured(-1);}return measured(read.status);
}
}
