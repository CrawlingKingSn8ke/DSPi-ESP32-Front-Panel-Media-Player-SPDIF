#pragma once
#include <stdint.h>

namespace PanelWifi {
enum class Owner:uint8_t {Setup,Network,Link,Transfer};
enum class State:uint8_t {Off,Joining,Online,Retrying,Setup,Stopping,Error};

// Pure policy. The production adapter alone performs radio/storage operations.
// First version intentionally makes the four service modes exclusive.
class Policy {
 public:
  State state()const{return state_;}
  bool owns(Owner owner)const{return leased_&&owner_==owner;}
  bool stopping(Owner owner)const{return state_==State::Stopping&&owner_==owner;}
  bool radioNeeded()const{return leased_||state_==State::Stopping;}
  bool acquire(Owner owner){
    if(leased_)return owner_==owner;
    if(state_!=State::Off)return false;
    owner_=owner;leased_=true;
    state_=owner==Owner::Setup?State::Setup:State::Joining;return true;
  }
  void release(Owner owner){
    if(!owns(owner))return;
    leased_=false;state_=State::Stopping;
  }
  void connected(){if(leased_&&owner_!=Owner::Setup)state_=State::Online;}
  void disconnected(){if(leased_&&owner_!=Owner::Setup)state_=State::Retrying;}
  void fail(){if(leased_)state_=State::Error;}
  void stopped(){if(!leased_&&state_==State::Stopping)state_=State::Off;}
 private:
  State state_=State::Off;Owner owner_=Owner::Network;bool leased_=false;
};
}
