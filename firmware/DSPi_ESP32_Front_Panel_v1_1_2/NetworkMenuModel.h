#pragma once
#include <cstdint>
namespace NetworkMenus {
inline bool usesNowPlaying(bool ownsNetwork,bool hasNetworkTrack,bool localActive){return ownsNetwork||(hasNetworkTrack&&!localActive);}
enum class Exclusive:uint8_t {None,Stop,Sd,Link,Transfer,Setup,Forget};
class Handover {
public:
 uint32_t ask(Exclusive action){if(!++serial_)++serial_;action_=action;draining_=false;return serial_;}
 bool confirm(uint32_t serial){if(serial!=serial_||action_==Exclusive::None||draining_)return false;draining_=true;return true;}
 void cancel(){action_=Exclusive::None;draining_=false;if(!++serial_)++serial_;}
 bool cancelPending(){if(draining_)return false;cancel();return true;}
 Exclusive finished(){if(!draining_)return Exclusive::None;auto action=action_;cancel();return action;}
 bool draining()const{return draining_;}
private:uint32_t serial_=0;Exclusive action_=Exclusive::None;bool draining_=false;
};
enum class RootAction:uint8_t {NowPlaying,Sd,Network,Playback,StopNetwork,Back};
inline uint8_t rootCount(bool current,bool armed){return uint8_t(4+(current?1:0)+(armed?1:0));}
inline RootAction rootAction(bool current,bool armed,unsigned row){
 if(current){if(!row)return RootAction::NowPlaying;--row;}
 if(row==0)return RootAction::Sd;if(row==1)return RootAction::Network;if(row==2)return RootAction::Playback;
 if(armed&&row==3)return RootAction::StopNetwork;return RootAction::Back;
}
inline const char* label(RootAction a){switch(a){case RootAction::NowPlaying:return "Now Playing";case RootAction::Sd:return "SD Card";case RootAction::Network:return "Network";case RootAction::Playback:return "Playback";case RootAction::StopNetwork:return "Stop Network";default:return "Back";}}
enum class BrowseAction:uint8_t {Entry,PreviousPage,NextPage,Back};
inline uint8_t browserCount(unsigned entries,bool previous,bool next){return uint8_t(entries+(previous?1:0)+(next?1:0)+1);}
inline BrowseAction browserAction(unsigned entries,bool previous,bool next,unsigned row){
 if(row<entries)return BrowseAction::Entry;row-=entries;
 if(previous){if(!row)return BrowseAction::PreviousPage;--row;}
 if(next&&!row)return BrowseAction::NextPage;return BrowseAction::Back;
}
}
