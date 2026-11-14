#pragma once
#include "SmbLibraryModel.h"
#include "SmbFileReader.h"
#include <atomic>
#include <cstdint>
struct smb2_context;
struct smb2fh;
namespace SmbLibrary {
struct Credentials {
 char server[254]={0},share[128]={0},user[128]={0},password[128]={0},domain[128]={0};
};
struct Identity {uint64_t inode=0,size=0,modified=0,nanoseconds=0;};
struct ReadStatistics {
 uint32_t calls=0,maxUs=0;uint64_t bytes=0,totalUs=0;
 uint64_t pollUs=0,serviceUs=0;uint32_t pollCalls=0,pollTimeouts=0,serviceMaxUs=0,errors=0,over100ms=0,over250ms=0,over500ms=0;
};
// One worker owns a client. Another task may only set its cancellation flag.
class Client:public FileReader {
public:
 explicit Client(const std::atomic<bool>& cancel):cancel_(cancel){}
 ~Client();
 Client(const Client&)=delete;Client& operator=(const Client&)=delete;
 bool connect(const Credentials&);
 bool list(const char* path,const char* after,Page& page);
 bool open(const char* path,Identity&);
 int readAt(uint64_t offset,uint8_t* bytes,size_t count)override;
 void close();
 const char* error()const{return error_;}
 ReadStatistics readStatistics()const{return {readCalls_.load(),readMaxUs_.load(),readBytes_.load(),readUs_.load(),readPollUs_.load(),readServiceUs_.load(),readPollCalls_.load(),readPollTimeouts_.load(),readServiceMaxUs_.load(),readErrors_.load(),readOver100ms_.load(),readOver250ms_.load(),readOver500ms_.load()};}
private:
 struct Reply;
 bool wait(Reply&);
 bool failed(const char*);
 const std::atomic<bool>& cancel_;smb2_context* context_=nullptr;smb2fh* file_=nullptr;
 char error_[96]={0};
 std::atomic<uint32_t> readCalls_{0},readMaxUs_{0};std::atomic<uint64_t> readBytes_{0},readUs_{0};
 // Only the owning task touches raw wait accumulators. Public read counters
 // contain readAt deltas, excluding connect/open/list operations.
 uint64_t waitPollUs_=0,waitServiceUs_=0;uint32_t waitPollCalls_=0,waitPollTimeouts_=0,currentServiceMaxUs_=0;
 std::atomic<uint64_t> readPollUs_{0},readServiceUs_{0};
 std::atomic<uint32_t> readPollCalls_{0},readPollTimeouts_{0},readServiceMaxUs_{0},readErrors_{0},readOver100ms_{0},readOver250ms_{0},readOver500ms_{0};
};
}
