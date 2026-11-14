#pragma once
#include "AudioDiagnostics.h"
#include <cstdint>
#include <cstring>
namespace NetworkRecovery {
class Gate {
 bool used_=false,pending_=false;
public:
 void used(){used_=true;}
 void request(){pending_=used_;}
 bool pending()const{return pending_;}
 bool needed()const{return used_;}
 bool ready(bool workersQuiet,bool outputQuiet,bool radioQuiet)const{return pending_&&workersQuiet&&outputQuiet&&radioQuiet;}
};
// UI destination only: no path, credentials, song position or autoplay.
struct BrowserReturn {uint32_t magic,inverse;};
inline void saveBrowser(BrowserReturn& r){r.magic=0x53444201u;r.inverse=~r.magic;}
inline bool takeBrowser(BrowserReturn& r,bool softwareReset){
 const bool valid=softwareReset&&r.magic==0x53444201u&&r.inverse==~r.magic;
 r={};return valid;
}
// No paths, credentials or playback intent survive the controlled restart.
struct Record {uint32_t magic;AudioDiagnostics::Snapshot sample;uint32_t checksum;};
inline uint32_t checksum(const AudioDiagnostics::Snapshot& s){
 const auto* bytes=reinterpret_cast<const unsigned char*>(&s);uint32_t hash=2166136261u;
 for(size_t i=0;i<sizeof s;++i)hash=(hash^bytes[i])*16777619u;return hash;
}
inline void save(Record& r,const AudioDiagnostics::Snapshot& s){r.magic=0;r.sample=s;r.checksum=checksum(r.sample);r.magic=0x534d4204u;}
inline bool take(Record& r,bool softwareReset,AudioDiagnostics::Snapshot& out){
 const bool valid=softwareReset&&r.magic==0x534d4204u&&r.checksum==checksum(r.sample);r.magic=0;
 if(valid)out=r.sample;return valid;
}
}
