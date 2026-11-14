#pragma once
#include "NetworkAudioOutput.h"
// Internal adapter: include after MediaPlayerPoC.h. Services contains only the
// main-loop UI/UART boundary; worker callbacks use the guarded PCM methods only.
template<class Media,class Services>class PanelNetworkOutput:public NetworkPlayer::Output {
public:
 explicit PanelNetworkOutput(Media& media):media_(media){}
 void stop(bool retained)override{Services::detachSd();if(retained)media_.requestTrackTransitionStop();else media_.requestStop();}
 bool cleanup()override{return media_.serviceStopCleanup()&&Services::storageQuiet();}
 bool begin(MediaByteSource& source,const NetworkAudioInfo& audio)override{
  MediaFileInfo info;
  switch(audio.format){case NetworkAudioInfo::Format::Wav:info.format=MediaFileFormat::Wav;break;case NetworkAudioInfo::Format::Flac:info.format=MediaFileFormat::Flac;break;case NetworkAudioInfo::Format::Mp3:info.format=MediaFileFormat::Mp3;break;default:return false;}
  info.sampleRate=audio.rate;info.channels=audio.channels;info.bitsPerSample=audio.bits;info.totalFrames=audio.frames;
  info.valid=true;info.nativeRateSupported=audio.rate==44100||audio.rate==48000;
  return media_.beginNetworkPlay(source,info,Services::pin,Services::routeCallback,nullptr,Services::log());
 }
 int start()override{const auto status=media_.servicePlayStart(Services::log(),0);return status==MediaStartStatus::Started?1:status==MediaStartStatus::Preparing?0:-1;}
 uint32_t pcmToken()const override{return media_.networkProducerToken();}
 size_t write(uint32_t token,const int32_t* pcm,size_t count)override{return media_.writeNetworkPcm(token,pcm,count);}
 void finish(uint32_t token,bool failed)override{media_.completeNetworkPcm(token,failed);}
 NetworkPlayer::OutputSnapshot snapshot()const override{
  using S=NetworkPlayer::OutputState;S state=S::Stopped;
  switch(media_.playbackState()){
   case MediaPlaybackState::Starting:state=S::Starting;break;
   case MediaPlaybackState::Playing:state=S::Playing;break;
   case MediaPlaybackState::Paused:case MediaPlaybackState::Seeking:state=S::Paused;break;
   case MediaPlaybackState::Draining:state=S::Stopping;break;
   case MediaPlaybackState::Finished:state=S::Finished;break;
   case MediaPlaybackState::Error:state=S::Error;break;default:break;
  }
  return {state,media_.playbackStats().outputFrames};
 }
 bool pause(bool paused)override{
  auto state=media_.playbackState();if((paused&&state==MediaPlaybackState::Paused)||(!paused&&state==MediaPlaybackState::Playing))return true;
  if(state!=MediaPlaybackState::Playing&&state!=MediaPlaybackState::Paused)return false;
  return media_.togglePause();
 }
 bool sourceMatches()const override{return Services::sourceMatches();}
 bool setVolume(uint8_t p)override{return Services::writeVolume(p);}
 bool setMute(bool value)override{return Services::writeMute(value);}
 void volume(float& db,bool& muted)const override{Services::volume(db,muted);}
private:Media& media_;
};
