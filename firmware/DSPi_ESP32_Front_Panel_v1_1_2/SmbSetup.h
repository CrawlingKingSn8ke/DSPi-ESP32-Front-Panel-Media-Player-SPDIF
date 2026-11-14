#pragma once
#include "SmbSetupModel.h"
#include "SmbBrowser.h"
namespace SmbLibrary {
// All methods are called under the enclosing setup policy mutex.
class Setup {
 public:
  Setup(){valid_=loadSettings(settings_);}
  unsigned request(const char* json){if(!valid_)return 503;if(pending_||browser_.busy())return 409;if(!parseSetupEdit(json,settings_,edit_)){state_="invalid";return 400;}pending_=true;started_=false;state_="checking";return 202;}
  void service(bool online){
   if(!pending_)return;
   if(edit_.remove){complete(saveSettings(edit_.settings));return;}
   if(!started_){if(!online){complete(false);return;}const auto& s=edit_.settings.shares[edit_.slot];started_=browser_.browse(s,s.root,"",1);if(!started_){complete(false);return;}}
   if(auto* r=browser_.take()){const bool okay=r->okay;Browser::freeResponse(r);complete(okay&&saveSettings(edit_.settings));}
  }
  bool list(char* out,size_t capacity){return setupList(settings_,valid_?state_:"unavailable",out,capacity);}
  bool busy()const{return pending_;}
  void cancel(){browser_.cancel();}
  bool stopped(){if(!browser_.stopped())return false;Browser::freeResponse(browser_.take());return true;}
 private:
  void complete(bool ok){if(ok)settings_=edit_.settings;memset(&edit_,0,sizeof edit_);pending_=false;state_=ok?"saved":"failed";}
  Settings settings_;SetupEdit edit_;Browser browser_;bool valid_=false,pending_=false,started_=false;const char* state_="ready";
};
}
