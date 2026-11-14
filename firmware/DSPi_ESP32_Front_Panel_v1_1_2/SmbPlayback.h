#pragma once
#include "SmbAudioJob.h"
#include "NetworkAudioOutput.h"
#include <string>
#include <algorithm>
#include <cstring>
namespace SmbPlayer {
struct Track {uint32_t share=0,revision=0;std::string path,title,album;};
enum class State:uint8_t {Stopped,Loading,Playing,Paused};
template<class Job>class Coordinator {
public:
 Coordinator(NetworkPlayer::Output& output,Job& job):output_(output),job_(job){}
 bool enter(){if(armed_||owns_||!job_.stopped())return false;armed_=true;error_[0]=0;return true;}
 void online(bool value){online_=value;}
 void leave(){armed_=false;stopPlayback();current_=Track{};credentials_=SmbLibrary::Credentials{};}
 void stopPlayback(){wantPlay_=false;next_=Track{};job_.cancel();if(owns_){output_.stop(false);stage_=Stage::WaitStop;}}
 bool play(const Track& track,const SmbLibrary::Credentials& credentials,uint32_t now){
  char checked[SmbLibrary::kPath+1];
  if(!armed_||!online_||!track.share||!track.revision||track.path.empty()||track.title.size()>256||track.album.size()>256||!SmbLibrary::join(track.path.c_str(),"probe",checked))return false;
  current_=track;next_=Track{};credentials_=credentials;wantPaused_=false;info_=NetworkAudioInfo{};replace(now,0,false);return true;
 }
 bool toggle(uint32_t now){
  if(!armed_||current_.path.empty())return false;
  if(stage_==Stage::Playing||stage_==Stage::Paused){
   const bool pause=stage_==Stage::Playing;if(!output_.pause(pause))return false;
   wantPaused_=pause;stage_=pause?Stage::Paused:Stage::Playing;return true;
  }
  if(stage_==Stage::Idle){wantPaused_=false;replace(now,0,false);return true;}
  wantPaused_=!wantPaused_;return true;
 }
 bool seek(uint32_t ms,uint32_t now){
  if(!seekable_||!owns_||(stage_!=Stage::Playing&&stage_!=Stage::Paused)||!info_.rate||(durationMs()&&ms>=durationMs()))return false;
  wantPaused_=stage_==Stage::Paused;replace(now,uint64_t(ms)*info_.rate/1000,true);return true;
 }
 bool next(uint32_t now){if(next_.path.empty())return false;current_=next_;next_=Track{};wantPaused_=false;replace(now,0,false);return true;}
 bool previous(uint32_t now){return seek(0,now);}
 bool setNext(const Track& track){char checked[SmbLibrary::kPath+1];if(track.share!=current_.share||track.revision!=current_.revision||track.path.empty()||!SmbLibrary::join(track.path.c_str(),"probe",checked))return false;next_=track;return true;}
 void clearNext(){next_=Track{};}
 void service(uint32_t now,bool backgroundQuiet=true){
  if(owns_&&(stage_==Stage::Playing||stage_==Stage::Paused)&&!output_.sourceMatches())leave();
  if(stage_==Stage::WaitStop){
   if(!output_.cleanup()||!job_.stopped()||!job_.release())return;
   if(!armed_||!wantPlay_){owns_=false;stage_=Stage::Idle;seekable_=false;firstFrame_=0;return;}
   if(!backgroundQuiet)return; // Old optional sockets must drain before new audio starts.
   if(!online_){fail("WiFi unavailable");return;}
   if(!job_.start(credentials_,current_.path.c_str(),generation_,output_,firstFrame_,verify_?&identity_:nullptr)){fail("Audio memory unavailable");return;}
   stage_=Stage::Preparing;started_=now;
  }
  if(stage_==Stage::Preparing||stage_==Stage::Prefill){
   if(uint32_t(now-started_)>=20000){fail("Network start timed out");return;}
   if(job_.failed()){fail(job_.error()[0]?job_.error():"Network file unavailable");return;}
  }
  if(stage_==Stage::Preparing&&job_.ready()){
   info_=job_.info();auto* source=job_.source();
   if(!source||job_.generation()!=generation_||!output_.begin(*source,info_)){fail("Audio output unavailable");return;}
   seekable_=source->seekable();job_.supplyPcmToken(output_.pcmToken());stage_=Stage::Prefill;
  }
  if(stage_==Stage::Prefill){const int result=output_.start();
   if(result<0){fail("Audio start failed");return;}
   if(result>0){stage_=Stage::Playing;if(wantPaused_){if(output_.pause(true))stage_=Stage::Paused;else fail("Pause failed");}}
  }
  if(stage_==Stage::Playing||stage_==Stage::Paused){
   auto output=output_.snapshot();
   if(job_.failed()||output.state==NetworkPlayer::OutputState::Error)fail(job_.failed()&&job_.error()[0]?job_.error():"Network playback failed");
   else if(output.state==NetworkPlayer::OutputState::Finished){if(!next_.path.empty())next(now);else stopPlayback();}
  }
 }
 bool active()const{return armed_;}bool ownsAudio()const{return owns_;}
 bool stopped()const{return !armed_&&!owns_&&job_.stopped()&&!job_.source();}
 bool seekable()const{return seekable_;}
 State state()const{return stage_==Stage::Playing?State::Playing:stage_==Stage::Paused?State::Paused:stage_==Stage::Idle?State::Stopped:State::Loading;}
 const Track& current()const{return current_;}const Track& queued()const{return next_;}
 uint32_t generation()const{return generation_;}
 uint64_t positionFrames()const{return owns_&&info_.rate?firstFrame_+output_.snapshot().frames:0;}
 bool seeking()const{return verify_&&state()==State::Loading;}
 uint32_t positionMs()const{return info_.rate?uint32_t(std::min<uint64_t>(UINT32_MAX,positionFrames()*1000/info_.rate)):0;}
 uint32_t durationMs()const{return info_.rate?uint32_t(std::min<uint64_t>(UINT32_MAX,info_.frames*1000/info_.rate)):0;}
 const char* error()const{return error_;}
private:
 enum class Stage:uint8_t{Idle,WaitStop,Preparing,Prefill,Playing,Paused};
 void replace(uint32_t now,uint64_t frame,bool verify){
  if(verify)identity_=job_.identity();verify_=verify;firstFrame_=frame;wantPlay_=true;error_[0]=0;
  if(!++generation_)++generation_;job_.cancel();output_.stop(true);owns_=true;stage_=Stage::WaitStop;started_=now;
 }
 void fail(const char* message){strncpy(error_,message,sizeof(error_)-1);error_[sizeof(error_)-1]=0;wantPlay_=false;job_.cancel();if(owns_){output_.stop(false);stage_=Stage::WaitStop;}else stage_=Stage::Idle;}
 NetworkPlayer::Output& output_;Job& job_;Track current_,next_;SmbLibrary::Credentials credentials_;
 SmbLibrary::Identity identity_;NetworkAudioInfo info_;Stage stage_=Stage::Idle;
 bool armed_=false,online_=false,owns_=false,wantPlay_=false,wantPaused_=false,verify_=false,seekable_=false;
 uint32_t started_=0,generation_=0;uint64_t firstFrame_=0;char error_[96]={0};
};
}
