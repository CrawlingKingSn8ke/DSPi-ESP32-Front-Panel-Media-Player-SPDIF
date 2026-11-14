#pragma once
#include <stdint.h>
namespace DspiLink {
enum class LinkPhase:uint8_t {Off,Preparing,Joining,Online,Reconnecting,Stopping,Error};
enum class ModeDecision:uint8_t {Start,ConfirmStopMusic,ConfirmStopLink,Busy};
class Lifecycle {
 public:
  LinkPhase phase()const{return phase_;}
  bool blocksMusic()const{return phase_!=LinkPhase::Off;}
  bool mayTransfer()const{return phase_==LinkPhase::Off;}
  void transfer(bool value){transfer_=value;}
  ModeDecision requestOn(bool media){
    if(transfer_||blocksMusic())return ModeDecision::Busy;
    if(media)return ModeDecision::ConfirmStopMusic;
    phase_=LinkPhase::Preparing;return ModeDecision::Start;
  }
  void confirmOn(){if(!transfer_&&phase_==LinkPhase::Off)phase_=LinkPhase::Preparing;}
  void cancel(){} // Unconfirmed entry has not acquired any resource.
  bool prepared(bool decoder,bool artwork,bool route,bool jobs){
    if(phase_!=LinkPhase::Preparing||!decoder||!artwork||!route||!jobs)return false;
    phase_=LinkPhase::Joining;return true;
  }
  ModeDecision requestMusic()const{return blocksMusic()?ModeDecision::ConfirmStopLink:ModeDecision::Start;}
  void connected(){if(phase_==LinkPhase::Joining||phase_==LinkPhase::Reconnecting)phase_=LinkPhase::Online;}
  void connectionLost(){if(phase_==LinkPhase::Online)phase_=LinkPhase::Reconnecting;}
  void stop(){if(blocksMusic())phase_=LinkPhase::Stopping;}
  void stopped(bool released){if(phase_==LinkPhase::Stopping&&released)phase_=LinkPhase::Off;}
  void fail(){phase_=LinkPhase::Error;}
 private:LinkPhase phase_=LinkPhase::Off;bool transfer_=false;
};
}
