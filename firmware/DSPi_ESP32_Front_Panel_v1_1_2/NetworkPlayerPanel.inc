#include "AudioDiagnostics.h"
#include "CoexistenceDiagnostics.h"
#include "NetworkPlaybackPolicy.h"
#include "NetworkRecovery.h"
#include "SmbBrowseScroll.h"
// Main-loop-only UI/UART integration; workers own SMB and decoding.
namespace NetworkPanel {
using Exclusive=NetworkMenus::Exclusive;
struct Services {
 static constexpr int pin=MEDIA_SPDIF_DATA_OUT_PIN;
 static Stream& log(){return Serial;}
 static bool routeCallback(uint32_t rate,void*){return mediaRouteCallback(rate,nullptr);}
 static void detachSd(){cancelMediaTrackTransition();cancelMediaArtworkRequest(true);mediaTerminalFinalizePending=false;mediaRouteRestorePending=false;mediaRouteRestoreRetryAt=0;mediaQueueCount=0;mediaQueueIndex=-1;mediaCurrentPath[0]=0;mediaPlaybackSuspended=false;}
 static bool sourceMatches(){return dspi.connected&&dspi.source==MEDIA_DSPI_SPDIF_SOURCE;}
 static bool storageQuiet(){return !__atomic_load_n(&mediaArtworkWorkerBusy,__ATOMIC_ACQUIRE);}
 static bool writeVolume(uint8_t){return false;}static bool writeMute(bool){return false;}
 static void volume(float& db,bool& muted){db=PanelHomeVolume::level(homeVolumeDisplay,true,dspi.volumeDb,dspi.masterVolumeDb);muted=PanelHomeVolume::muted(homeVolumeDisplay,true,dspi.muted,dspi.masterVolumeDb);}
};
struct Radio {
 bool begin(const char* ssid,const char* password,const char* host){return PanelWifi::beginStation(PanelWifi::Owner::Network,ssid,password,host);}
 void service(uint32_t now){PanelWifi::service(now);}
 bool connected()const{return PanelWifi::owns(PanelWifi::Owner::Network)&&PanelWifi::snapshot().stationConnected;}
 bool release(){return PanelWifi::release(PanelWifi::Owner::Network);}
};
struct SdRequest{int index=-1,direction=0;bool wrap=false,skip=false,automatic=false,preserve=false;};
struct State {
 PanelNetworkOutput<MediaPlayerPoC,Services> output;SmbLibrary::AudioJob audio;SmbPlayer::Coordinator<SmbLibrary::AudioJob> core;Radio radio;
 SmbLibrary::Settings settings;SmbLibrary::Browser browser;SmbLibrary::Navigation nav;SmbLibrary::BrowseResponse* page=nullptr;
 SmbLibrary::Artwork artwork;uint16_t* artPixels=nullptr;std::string artFolder,queueFolder,queueAfter,queueTrack;
 uint32_t artShare=0,artRevision=0,artGeneration=0,artChangedAt=0;bool artAttempted=false;
 NetworkResources resources;NetworkPlayback::BackgroundGate background;NetworkMenus::Handover handover;uint32_t handoverToken=0,generation=0,queueGeneration=0,queueJobGeneration=0;
 bool started=false,failed=false,closing=false,online=false,setupReturn=false,wantBrowse=false,queuePending=false,queueWanted=false;
 bool settingsOkay=false;unsigned selected=0,restoreRow=1;uint32_t startedAt=0,redrawAt=0;
 Exclusive confirmAction=Exclusive::None;
 uint8_t observedSource=uint8_t(dspi.source);MenuPage returnPage=PAGE_MUSIC_ROOT;SdRequest sd;char lastError[96]={0};
 uint32_t presentedGeneration=0;SmbPlayer::State presentedState=SmbPlayer::State::Stopped;uint16_t* presentedArt=nullptr;
 uint32_t diagnosticAt=0,diagnosticGeneration=0,diagnosticStartedAt=0,resourceAt=0;
 uint32_t internal=0,largest=0,psram=0;int rssi=-127,sleep=-1;
 State():output(mediaPlayerPoc),core(output,audio){settingsOkay=SmbLibrary::loadSettings(settings);}
};
static State* state=nullptr;
static NetworkRecovery::Gate recovery;
static bool pendingNetwork=false,returnToSd=false;
RTC_NOINIT_ATTR static unsigned char retainedBrowser[sizeof(NetworkRecovery::BrowserReturn)];
static WifiSignal::Sampler signalSampler;
static unsigned signalLevel=0;static uint32_t signalPaintAt=0;
static bool recoverySaving=false,recoverySaveWarned=false;
static uint32_t recoverySaveAt=0;
static bool saveRecoveryPreferences(){
 if(!pendingDeferredPreferenceFlags())return true;
 if(!recoverySaving){
  recoverySaving=true;recoverySaveAt=millis();showToast("Saving settings");lastUserActivityAt=millis()-2000U;
  portENTER_CRITICAL(&deferredPreferenceMux);deferredPreferenceNotBefore=millis();portEXIT_CRITICAL(&deferredPreferenceMux);
 }
 serviceDeferredPreferences();
 if(!pendingDeferredPreferenceFlags())return true;
 if(!recoverySaveWarned&&uint32_t(millis()-recoverySaveAt)>=8000){recoverySaveWarned=true;showToast("Settings save pending");}
 return false;
}
static State* get(){if(!state){void* p=heap_caps_malloc(sizeof(State),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(p)state=new(p)State;}return state;}
static String text(const char* value){return LinkPanel::displayText(value?value:"");}
static void refresh(){if(uiView==VIEW_MENU&&networkOwnsPage(menuPage)&&!editActive)drawMenu();}
static bool ready(){return state&&state->started&&!state->closing&&state->core.active()&&state->radio.connected();}
static const SmbLibrary::Share* currentShare(){if(!state)return nullptr;const auto& track=state->core.current();for(const auto& s:state->settings.shares)if(s.id==track.share&&s.revision==track.revision)return &s;return nullptr;}
static unsigned shareCount(){unsigned n=0;if(state)for(const auto& s:state->settings.shares)if(s.id)++n;return n;}
static unsigned slotAt(unsigned row){for(unsigned i=0;i<SmbLibrary::kShares;++i)if(state->settings.shares[i].id){if(!row)return i;--row;}return SmbLibrary::kShares;}
static void clearPage(){if(state){SmbLibrary::Browser::freeResponse(state->page);state->page=nullptr;}}
static void stop(){if(!state)return;state->closing=true;state->core.leave();state->browser.cancel();state->artwork.cancel();++state->generation;state->wantBrowse=false;state->queueWanted=false;}
static void ask(Exclusive action){auto* s=get();if(!s){showToast("Network memory unavailable");return;}if(s->handover.draining()||s->closing){showToast("Stopping Network...");return;}s->confirmAction=action;s->returnPage=networkOwnsPage(menuPage)?menuPage:PAGE_MUSIC_ROOT;s->handoverToken=s->handover.ask(action);enterPage(PAGE_NETWORK_CONFIRM);}
static void openShareSetup(){auto* s=get();if(!s)return;s->setupReturn=true;LinkPanel::openSetup();}
static bool open(){
 if(!linkAllowMusic())return false;auto* s=get();if(!s){showToast("Network memory unavailable");return false;}
 if(!s->started&&(mediaPlayerPoc.active()||mediaTrackTransitionActive()||!Services::storageQuiet())){
  Services::detachSd();mediaPlayerPoc.requestStop();pendingNetwork=true;showToast("Stopping SD...");return false;
 }
 if(!s->settingsOkay){showToast("Share storage unavailable");return false;}
 if(!s->started){DspiLink::LinkNetworkSettings settings;DspiLink::loadLinkSettings(settings);
  if(!settings.ssid[0]){memset(settings.password,0,sizeof settings.password);openShareSetup();return false;}
  const bool okay=s->core.enter()&&s->radio.begin(settings.ssid,settings.password,settings.bridge.hostname);memset(settings.password,0,sizeof settings.password);
  if(!okay){s->core.leave();s->radio.release();showToast("WiFi is busy");return false;}
  s->started=true;s->closing=false;s->failed=false;s->startedAt=millis();s->observedSource=uint8_t(dspi.source);recovery.used();
  AudioDiagnostics::event(AudioDiagnostics::Event::NetworkStarted,millis());
 }
 enterPage(PAGE_NETWORK_SERVERS);return true;
}
static void requestBrowse(){auto& s=*state;s.browser.cancel();++s.generation;s.wantBrowse=true;s.restoreRow=1;clearPage();mediaBrowserTopRow=0;resetMediaMarquee();}
static SmbPlayer::Track track(const SmbLibrary::Entry& e,const char* parent,const SmbLibrary::Share& share){SmbPlayer::Track t;char path[SmbLibrary::kPath+1];if(!SmbLibrary::join(parent,e.name,path))return t;t.share=share.id;t.revision=share.revision;t.path=path;t.title=e.name;const char* slash=strrchr(parent,'/');t.album=slash?slash+1:parent;return t;}
static std::string folderOf(const std::string& path){const auto at=path.rfind('/');return at==std::string::npos?std::string():path.substr(0,at);}
static std::string nameOf(const std::string& path){const auto at=path.rfind('/');return at==std::string::npos?path:path.substr(at+1);}
static bool bufferHealthy(){return state->background.allowed(mediaPlayerPoc.ringCapacityFrames(),mediaPlayerPoc.bufferedFrames(),millis());}
static void serviceBackgroundTransition(uint32_t now){
 auto& s=*state;const auto phase=s.core.state();
 const bool changed=s.background.observe(s.core.generation(),phase==SmbPlayer::State::Playing,phase==SmbPlayer::State::Paused,now);
 if(!changed&&phase!=SmbPlayer::State::Loading)return;
 if(s.queuePending)s.browser.cancel();
 if(!s.artwork.stopped()){
  s.artwork.cancel();
  if(changed&&!s.artPixels)s.artAttempted=false;
 }
}
static void serviceBackgroundPressure(){
 auto& s=*state;if(!s.core.ownsAudio()||s.core.state()==SmbPlayer::State::Paused)return;
 if(!NetworkPlayback::backgroundPressure(mediaPlayerPoc.ringCapacityFrames(),mediaPlayerPoc.bufferedFrames()))return;
 // A queue lookup may retry after the reserve recovers. Artwork is attempted
 // only once per album, so cancelling it cannot create a download/retry loop.
 if(s.queuePending)s.browser.cancel();
 if(!s.artwork.stopped())s.artwork.cancel();
}
static void serviceQueue(){auto& s=*state;const auto& t=s.core.current();
 if(s.queueTrack!=t.path||s.queueGeneration!=s.core.generation()){s.queueGeneration=s.core.generation();s.queueTrack=t.path;s.queueFolder=folderOf(t.path);s.queueAfter=nameOf(t.path);s.queueWanted=!t.path.empty();}
 if(!s.queueWanted||!ready()||s.wantBrowse||s.browser.busy()||s.queuePending||!bufferHealthy()||!s.core.queued().path.empty())return;
 if(const auto* share=currentShare())if(s.browser.browse(*share,s.queueFolder.c_str(),s.queueAfter.c_str(),++s.generation)){s.queueJobGeneration=s.core.generation();s.queuePending=true;s.queueWanted=false;}
}
static bool queueResponseCurrent(const SmbLibrary::BrowseResponse& r){auto& s=*state;const auto& t=s.core.current();
 const bool current=!r.cancelled&&r.generation==s.generation&&r.share==t.share&&r.revision==t.revision&&s.queueJobGeneration==s.core.generation()&&s.queueTrack==t.path;
 if(!current&&!s.closing&&!t.path.empty()&&s.core.queued().path.empty())s.queueWanted=true;
 return current;
}
static void serviceArt(uint32_t now){auto& s=*state;const auto& t=s.core.current();const auto folder=folderOf(t.path);
 if(folder!=s.artFolder||t.share!=s.artShare||t.revision!=s.artRevision){s.artFolder=folder;s.artShare=t.share;s.artRevision=t.revision;++s.artGeneration;s.artwork.cancel();free(s.artPixels);s.artPixels=nullptr;s.artChangedAt=now;s.artAttempted=false;}
 uint32_t generation=0;auto* pixels=s.artwork.take(generation);if(pixels){if(generation==s.artGeneration&&!s.closing){free(s.artPixels);s.artPixels=pixels;}else free(pixels);}
 if(!s.core.ownsAudio()||s.artAttempted||!s.artwork.stopped()||uint32_t(now-s.artChangedAt)<1500||!bufferHealthy())return;
 if(const auto* share=currentShare()){s.artAttempted=true;s.artwork.start(*share,s.artFolder.c_str(),s.artGeneration,t.path.c_str());}
}
}
bool networkOwnsPage(MenuPage p){return p==PAGE_MUSIC_ROOT||p==PAGE_NETWORK_SERVERS||p==PAGE_NETWORK_BROWSER||p==PAGE_NETWORK_CONFIRM||p==PAGE_WIFI_SIGNAL;}
bool networkDrawSignal(){using namespace NetworkPanel;
 if(menuPage!=PAGE_WIFI_SIGNAL)return false;
 const bool connected=PanelWifi::snapshot().stationConnected;
 if(!connected){enterPage(PAGE_WIFI);return true;}
 if(signalSampler.due(millis(),true,connected))signalLevel=WifiSignal::bars(PanelWifi::signalStrength());
 signalPaintAt=millis();uiView=VIEW_MENU;drawBase();
 drawFontCentredGlow(FontMedium,8,"WiFi Signal");drawTaperLine(UI_W/2,44,210,1,uiAccentDark());
 WifiSignal::draw([](int x,int y,int w,int h,uint16_t c){canvas->fillRect(x,y,w,h,c);},80,166,signalLevel,uiAccent(),uiAccentDark());
 drawFontCentred(FontMedium,194,"Back",uiMainText());flushCanvasLocked();return true;
}
void networkRestoreBrowser(){NetworkRecovery::BrowserReturn record;memcpy(&record,NetworkPanel::retainedBrowser,sizeof record);memset(NetworkPanel::retainedBrowser,0,sizeof record);if(NetworkRecovery::takeBrowser(record,esp_reset_reason()==ESP_RST_SW))enterPage(PAGE_MEDIA);}
bool networkMoveMenu(int direction){using namespace NetworkPanel;
 if(menuPage==PAGE_WIFI_SIGNAL)return true;
 if(menuPage!=PAGE_NETWORK_BROWSER)return false;
 if(!state||!state->page||state->wantBrowse||(state->browser.busy()&&!state->queuePending))return false;
 const auto& page=state->page->page;const auto move=SmbLibrary::scroll(menuIndex,page.count,direction,state->nav.hasPrevious(),page.more);
 if(move.page){bool okay=move.page<0?state->nav.previous():state->nav.next(page.entries[page.count-1].name);
  if(okay){requestBrowse();state->restoreRow=move.row;}else showToast("Browse memory unavailable");
 }else menuIndex=move.row;
 rememberedMenuIndex[menuPage]=menuIndex;requestMediaBrowserNavigationRedraw(move.page!=0);return true;
}
bool networkBrowserEntry(uint8_t row,const char*& name,bool& directory){using namespace NetworkPanel;
 name="";directory=false;if(!state||state->wantBrowse||!state->page||(!state->queuePending&&state->browser.busy())||!row||row>state->page->page.count)return false;
 const auto& entry=state->page->page.entries[row-1];name=entry.name;directory=entry.directory;return true;
}
void networkBrowserContext(const char*& path,uint32_t& first,uint32_t& total){using namespace NetworkPanel;
 path=state?state->nav.path():"";first=state?state->nav.pageIndex()*SmbLibrary::kPage:0;total=state&&state->page?state->page->page.total:0;
}
bool networkOwnsAudio(){return NetworkPanel::state&&NetworkPanel::state->core.ownsAudio();}
bool networkIsArmed(){return NetworkPanel::state&&NetworkPanel::state->started;}
bool networkHasTrack(){return NetworkPanel::state&&NetworkPanel::state->core.active()&&!NetworkPanel::state->core.current().path.empty();}
bool networkUsesNowPlaying(){return NetworkMenus::usesNowPlaying(networkOwnsAudio(),networkHasTrack(),mediaPlayerPoc.active());}
bool networkWantsShareSetup(){return NetworkPanel::state&&NetworkPanel::state->setupReturn;}
String networkPageTitle(MenuPage p){return p==PAGE_MUSIC_ROOT?"Music Library":p==PAGE_NETWORK_CONFIRM?(NetworkPanel::state&&NetworkPanel::state->confirmAction==NetworkMenus::Exclusive::Sd?"Restart for SD?":"Stop Network?"):p==PAGE_NETWORK_BROWSER?"Network Folder":"Network Music";}
uint8_t networkMenuCount(MenuPage p){using namespace NetworkPanel;if(p==PAGE_WIFI_SIGNAL)return 1;if(p==PAGE_MUSIC_ROOT)return NetworkMenus::rootCount(networkHasTrack()||mediaCurrentPath[0]||mediaPlayerPoc.active(),networkIsArmed());if(p==PAGE_NETWORK_CONFIRM)return 2;if(p==PAGE_NETWORK_SERVERS)return uint8_t(shareCount()+2);if(!state||state->wantBrowse||(state->browser.busy()&&!state->queuePending))return 2;return state->page?uint8_t(state->page->page.count+1):2;}
String networkMenuName(MenuPage p,uint8_t row){using namespace NetworkPanel;
 if(p==PAGE_MUSIC_ROOT)return NetworkMenus::label(NetworkMenus::rootAction(networkHasTrack()||mediaCurrentPath[0]||mediaPlayerPoc.active(),networkIsArmed(),row));
 if(p==PAGE_NETWORK_CONFIRM)return row==0?"Cancel":state&&state->handover.draining()?"Stopping...":state&&state->confirmAction==Exclusive::Sd?"Restart to SD":"Continue";
 if(p==PAGE_NETWORK_SERVERS){unsigned count=shareCount();if(row<count)return text(state->settings.shares[slotAt(row)].label);return row==count?"Network Setup":"Back";}
 if(!state)return row==0?"Retry":"Back";if(state->wantBrowse||(state->browser.busy()&&!state->queuePending))return row==0?(ready()?"Loading...":"Connecting..."):"Back";if(!state->page)return row==0?"Retry":"Back";
 const auto& page=state->page->page;return row&&row<=page.count?text(page.entries[row-1].name):"Back";
}
String networkMenuValue(MenuPage p,uint8_t row){using namespace NetworkPanel;if(p==PAGE_MUSIC_ROOT){auto a=NetworkMenus::rootAction(networkHasTrack()||mediaCurrentPath[0]||mediaPlayerPoc.active(),networkIsArmed(),row);return a==NetworkMenus::RootAction::StopNetwork?"Confirm":a==NetworkMenus::RootAction::Back?"":"Open";}if(p==PAGE_NETWORK_SERVERS)return row<shareCount()?"Open":row==shareCount()?"Edit":"";if(p==PAGE_NETWORK_BROWSER&&state&&state->page&&row&&row<=state->page->page.count)return state->page->page.entries[row-1].directory?"Open":"Play";return "";}
bool networkMenuPlaying(MenuPage p,uint8_t row){using namespace NetworkPanel;if(!state||!networkHasTrack())return false;const auto& t=state->core.current();if(p==PAGE_NETWORK_SERVERS){auto slot=slotAt(row);return slot<SmbLibrary::kShares&&state->settings.shares[slot].id==t.share;}if(p!=PAGE_NETWORK_BROWSER||!state->page||!row||row>state->page->page.count||state->page->share!=t.share)return false;const auto& e=state->page->page.entries[row-1];char path[SmbLibrary::kPath+1];if(!SmbLibrary::join(state->nav.path(),e.name,path))return false;const size_t n=strlen(path);return e.directory?t.path.size()>n&&t.path.compare(0,n,path)==0&&t.path[n]=='/':t.path==path;}
bool networkSelect(){using namespace NetworkPanel;if(!networkOwnsPage(menuPage)||editActive)return false;
 if(menuPage==PAGE_WIFI_SIGNAL){enterPage(PAGE_WIFI);return true;}
 if(menuPage==PAGE_MUSIC_ROOT){switch(NetworkMenus::rootAction(networkHasTrack()||mediaCurrentPath[0]||mediaPlayerPoc.active(),networkIsArmed(),menuIndex)){case NetworkMenus::RootAction::NowPlaying:transitionToMediaNowPlaying();break;case NetworkMenus::RootAction::Sd:pendingNetwork=false;if(networkIsArmed()||recovery.needed())ask(Exclusive::Sd);else enterPage(PAGE_MEDIA);break;case NetworkMenus::RootAction::Network:open();break;case NetworkMenus::RootAction::Playback:enterPage(PAGE_MEDIA_SETTINGS);break;case NetworkMenus::RootAction::StopNetwork:ask(Exclusive::Stop);break;default:enterPage(PAGE_MAIN);}return true;}
 if(menuPage==PAGE_NETWORK_CONFIRM){if(!state||state->handover.draining())return true;if(menuIndex==0){state->handover.cancelPending();enterPage(state->returnPage);}else if(state->handover.confirm(state->handoverToken)){if(state->confirmAction==Exclusive::Sd)returnToSd=true;if(state->started)stop();else{state->handover.finished();recovery.request();}drawMenu();}return true;}
 if(menuPage==PAGE_NETWORK_SERVERS){unsigned count=shareCount();if(menuIndex<count){const auto slot=slotAt(menuIndex);if(!state->started&&!open())return true;if(!state->nav.reset(state->settings.shares[slot].root)){showToast("Browse memory unavailable");return true;}state->selected=slot;requestBrowse();enterPage(PAGE_NETWORK_BROWSER);}else if(menuIndex==count)openShareSetup();else enterPage(PAGE_MUSIC_ROOT);return true;}
 if(!state)return true;if(state->wantBrowse||(state->browser.busy()&&!state->queuePending)){if(menuIndex==1)networkBack();return true;}if(!state->page){if(!menuIndex)requestBrowse();else networkBack();refresh();return true;}
 const auto& page=state->page->page;
 if(!menuIndex||menuIndex>page.count){networkBack();return true;}
 const auto e=page.entries[menuIndex-1];if(e.directory){if(state->nav.push(e.name,menuIndex))requestBrowse();else showToast("Folder limit reached");refresh();}else{auto t=track(e,state->nav.path(),state->settings.shares[state->selected]);if(state->core.play(t,state->settings.shares[state->selected].connection,millis()))transitionToMediaNowPlaying();else showToast("Network unavailable");}return true;
}
bool networkBack(){using namespace NetworkPanel;if(uiView==VIEW_MEDIA_NOW_PLAYING&&networkUsesNowPlaying()){enterPage(state&&state->page?PAGE_NETWORK_BROWSER:PAGE_MUSIC_ROOT);return true;}if(uiView!=VIEW_MENU||editActive)return false;if(menuPage==PAGE_WIFI_SIGNAL){enterPage(PAGE_WIFI);return true;}if(menuPage==PAGE_MEDIA&&!mediaBrowserDepth){enterPage(PAGE_MUSIC_ROOT);return true;}if(!networkOwnsPage(menuPage))return false;pendingNetwork=false;if(state){if(!state->handover.cancelPending())return true;if(menuPage==PAGE_NETWORK_BROWSER){unsigned row=0;if(state->nav.pop(row)){requestBrowse();state->restoreRow=row;refresh();return true;}state->browser.cancel();state->wantBrowse=false;++state->generation;}}enterPage(menuPage==PAGE_MUSIC_ROOT?PAGE_MAIN:menuPage==PAGE_NETWORK_BROWSER?PAGE_NETWORK_SERVERS:PAGE_MUSIC_ROOT);return true;}
void networkCancelPending(){NetworkPanel::pendingNetwork=false;if(NetworkPanel::state){NetworkPanel::state->handover.cancelPending();NetworkPanel::state->setupReturn=false;}}
bool networkAllowExclusive(NetworkMenus::Exclusive a){NetworkPanel::pendingNetwork=false;if(!networkIsArmed())return true;NetworkPanel::ask(a);return false;}
bool networkAllowSdTrack(int index,int direction,bool wrap,bool skip,bool automatic,bool preserve){NetworkPanel::pendingNetwork=false;if(!networkIsArmed()){NetworkPanel::recovery.request();return !NetworkPanel::recovery.pending();}if(automatic&&!networkOwnsAudio())return true;NetworkPanel::state->sd={index,direction,wrap,skip,automatic,preserve};NetworkPanel::ask(NetworkMenus::Exclusive::Sd);return false;}
bool networkSetupFinished(bool saved){auto* s=NetworkPanel::state;if(!s||!s->setupReturn)return false;s->setupReturn=false;s->settingsOkay=SmbLibrary::loadSettings(s->settings);if(saved)NetworkPanel::open();else enterPage(PAGE_MUSIC_ROOT);return true;}
void networkBeforeInputChange(){NetworkPanel::pendingNetwork=false;if(networkIsArmed())NetworkPanel::stop();}
bool networkMediaGuardQuiet(){using namespace NetworkPanel;
 return !state||(state->core.state()==SmbPlayer::State::Stopped&&!state->core.ownsAudio()&&
   state->audio.stopped()&&!state->audio.source()&&state->browser.stopped()&&state->artwork.stopped());
}
bool networkStopForMediaGuard(){using namespace NetworkPanel;
 if(!state)return true;auto& s=*state;
 // Notify the owner before stopping its shared lower-level output. Keep the
 // station and cached cover, but never leave a live decoder marked Playing.
 s.core.stopPlayback();s.browser.cancel();s.artwork.cancel();s.wantBrowse=false;s.queueWanted=false;
 const uint32_t started=millis();
 do{
  s.core.service(millis(),false);
  if(s.browser.stopped()){SmbLibrary::Browser::freeResponse(s.browser.take());s.queuePending=false;}
  uint32_t generation=0;free(s.artwork.take(generation));
  if(networkMediaGuardQuiet()){if(!s.artPixels)s.artAttempted=false;return true;}
  delay(2);
 }while(uint32_t(millis()-started)<5000);
 return false; // Flash admission must remain closed if a callback is still live.
}
bool networkTogglePlayback(){return networkHasTrack()&&NetworkPanel::state->core.toggle(millis());}
bool networkSkip(int direction){if(!networkHasTrack())return false;auto& c=NetworkPanel::state->core;if(direction<0)return c.previous(millis());if(c.next(millis()))return true;showToast("No next track ready");return false;}
bool networkSeekRelative(int direction,uint16_t seconds){if(!networkHasTrack())return false;auto& c=NetworkPanel::state->core;if(!c.seekable()){showToast("Seek unavailable");return false;}int64_t target=int64_t(c.positionMs())+(direction<0?-1:1)*int64_t(seconds)*1000;target=std::max<int64_t>(0,target);if(c.durationMs()&&target>=c.durationMs())target=c.durationMs()-1;return c.seek(uint32_t(target),millis());}
bool networkMediaPresentation(NetworkMediaPresentation& view){
 using namespace NetworkPanel;if(!state||!networkUsesNowPlaying())return false;
 const auto& current=state->core.current();view.path=current.path.c_str();view.album=current.album.c_str();
 view.artwork=state->artPixels;view.positionFrames=state->core.positionFrames();view.error=state->core.error();
 view.loading=state->core.state()==SmbPlayer::State::Loading;view.seeking=state->core.seeking();return true;
}
void serviceNetworkPanel(){using namespace NetworkPanel;const uint32_t now=millis();
 const bool signalVisible=uiView==VIEW_MENU&&menuPage==PAGE_WIFI_SIGNAL;
 if(signalVisible&&(!PanelWifi::snapshot().stationConnected||uint32_t(now-signalPaintAt)>=1000))drawMenu();
 if(!signalVisible)signalSampler.due(now,false,false);
 if(pendingNetwork&&mediaPlayerPoc.serviceStopCleanup()&&Services::storageQuiet()&&!mediaTrackTransitionActive()){
  pendingNetwork=false;open();
 }
 if(!state)return;auto& s=*state;
 if(s.started&&(!s.resourceAt||uint32_t(now-s.resourceAt)>=1000)){
  s.resourceAt=now;s.internal=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);s.largest=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);s.psram=heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  s.rssi=PanelWifi::signalStrength();s.sleep=PanelWifi::powerSaveMode();
  if(s.resources.observe(now,s.internal,s.largest,s.psram,s.started)){const auto stats=mediaPlayerPoc.playbackStats();Serial.printf("SMB MEM: internal=%u min=%u largest=%u min=%u psram=%u min=%u ring=%u/%u underruns=%u errors=%u output_stack_free=%u loop_stack_free=%u\r\n",unsigned(s.internal),unsigned(s.resources.minimumInternal()),unsigned(s.largest),unsigned(s.resources.minimumLargest()),unsigned(s.psram),unsigned(s.resources.minimumPsram()),unsigned(mediaPlayerPoc.bufferedFrames()),unsigned(mediaPlayerPoc.ringCapacityFrames()),unsigned(stats.underrunEvents),unsigned(stats.spdifErrors),unsigned(stats.outputStackMinFree),unsigned(uxTaskGetStackHighWaterMark(nullptr)));}
 }
 // A ready decoder can precede beginNetworkPlay(). Until its new producer
 // token exists, output statistics may still describe the previous song.
 if(s.audio.ready()&&s.audio.generation()==s.core.generation()&&s.output.pcmToken()!=0&&uint32_t(now-s.diagnosticAt)>=250){
  s.diagnosticAt=now;const auto info=s.audio.info();const auto t=s.audio.telemetry();const auto output=mediaPlayerPoc.playbackStats();
  if(s.diagnosticGeneration!=s.audio.generation()){s.diagnosticGeneration=s.audio.generation();s.diagnosticStartedAt=now;}
  AudioDiagnostics::NetworkSample sample;sample.rate=info.rate;sample.bits=info.bits;
  sample.buffered=mediaPlayerPoc.bufferedFrames();sample.capacity=mediaPlayerPoc.ringCapacityFrames();sample.underruns=output.underrunEvents;
  sample.compressedReady=t.compressedReady;sample.compressedCapacity=t.compressedCapacity;
  sample.readCalls=t.reads.calls;sample.readMaxUs=t.reads.maxUs;sample.readBytes=t.reads.bytes;sample.readUs=t.reads.totalUs;
  sample.pollUs=t.reads.pollUs;sample.serviceUs=t.reads.serviceUs;sample.pollCalls=t.reads.pollCalls;sample.pollTimeouts=t.reads.pollTimeouts;sample.serviceMaxUs=t.reads.serviceMaxUs;sample.readErrors=t.reads.errors;
  sample.readOver100ms=t.reads.over100ms;sample.readOver250ms=t.reads.over250ms;sample.readOver500ms=t.reads.over500ms;
  sample.decodeCalls=t.decodeCalls;sample.decodeMaxUs=t.decodeMaxUs;sample.computeMaxUs=t.computeMaxUs;sample.decodedFrames=t.decodedFrames;
  sample.generation=s.audio.generation();sample.elapsedMs=now-s.diagnosticStartedAt;sample.outputFrames=output.outputFrames;sample.outputErrors=output.spdifErrors;sample.underrunFrames=output.underrunFrames;
  const auto& path=s.core.current().path;sample.trackHash=2166136261u;for(unsigned char c:path)sample.trackHash=(sample.trackHash^c)*16777619u;
  const auto name=nameOf(path);snprintf(sample.track,sizeof sample.track,"%s",name.c_str());
  sample.freeInternal=s.internal;sample.largestInternal=s.largest;sample.minimumInternal=s.resources.minimumInternal();sample.minimumLargest=s.resources.minimumLargest();sample.freePsram=s.psram;sample.rssi=s.rssi;sample.sleep=s.sleep;
  sample.phase=uint8_t(s.core.state());sample.jobs=(s.queuePending?1:0)|(!s.artwork.stopped()?2:0)|(!s.queuePending&&s.browser.busy()?4:0);AudioDiagnostics::networkSample(sample);
  CoexistenceDiagnostics::observeNetwork(sample,now);
 }
 if(s.started&&uint8_t(dspi.source)!=s.observedSource&&dspi.source!=MEDIA_DSPI_SPDIF_SOURCE)stopMediaForInputChange("network input change");s.observedSource=uint8_t(dspi.source);
 if(s.started){s.radio.service(now);s.online=s.radio.connected();s.core.online(s.online);if(s.online)s.startedAt=now;else if(uint32_t(now-s.startedAt)>=30000&&!s.closing){s.failed=true;stop();showToast("WiFi unavailable");}}
 serviceBackgroundTransition(now);
 s.core.service(now,(!s.queuePending||s.browser.stopped())&&s.artwork.stopped());
 serviceBackgroundTransition(now); // Natural next-track advance can change generation in service().
 if(auto* response=s.browser.take()){const bool queued=s.queuePending;s.queuePending=false;const bool currentQueue=!queued||queueResponseCurrent(*response);
  if(!s.closing&&response->okay&&response->generation==s.generation&&currentQueue){if(queued){if(response->share==s.core.current().share&&response->revision==s.core.current().revision&&s.queueTrack==s.core.current().path){bool found=false;for(unsigned i=0;i<response->page.count;++i)if(!response->page.entries[i].directory){if(const auto* share=currentShare())found=s.core.setNext(track(response->page.entries[i],response->path,*share));break;}if(!found&&response->page.more&&response->page.count){s.queueAfter=response->page.entries[response->page.count-1].name;s.queueWanted=true;}}}else if(response->share==s.settings.shares[s.selected].id&&!strcmp(response->path,s.nav.path())&&!strcmp(response->after,s.nav.after())){clearPage();s.page=response;response=nullptr;menuIndex=std::min<unsigned>(s.restoreRow,s.page->page.count);s.restoreRow=1;}}
  else if(!s.closing&&!queued&&response->generation==s.generation&&!response->cancelled)showToast("Share unavailable");SmbLibrary::Browser::freeResponse(response);refresh();}
 if(s.wantBrowse&&ready()&&!s.browser.busy()){s.wantBrowse=false;const auto& share=s.settings.shares[s.selected];if(s.browser.browse(share,s.nav.path(),s.nav.after(),++s.generation))s.queuePending=false;else showToast("Share busy");refresh();}
 serviceBackgroundPressure();serviceQueue();serviceArt(now);
 if(s.core.error()[0]&&strcmp(s.lastError,s.core.error())){strlcpy(s.lastError,s.core.error(),sizeof s.lastError);showToast(text(s.lastError));}else if(!s.core.error()[0])s.lastError[0]=0;
 if(uiView==VIEW_MEDIA_NOW_PLAYING&&!mediaVolumeOverlayVisible&&(s.presentedGeneration!=s.core.generation()||s.presentedState!=s.core.state()||s.presentedArt!=s.artPixels)){
  s.presentedGeneration=s.core.generation();s.presentedState=s.core.state();s.presentedArt=s.artPixels;drawMediaNowPlaying();
 }
 if(s.started&&s.closing&&s.core.stopped()&&s.browser.stopped()&&s.artwork.stopped()){if(!s.radio.release())return;s.started=false;s.online=false;s.closing=false;clearPage();free(s.artPixels);s.artPixels=nullptr;s.artFolder.clear();s.artShare=0;s.queueTrack.clear();s.queuePending=false;const auto action=s.handover.finished();if(action==Exclusive::Link){enterPage(PAGE_LINK);LinkPanel::start();}else if(action==Exclusive::Transfer)beginWifiTransferConfirmation();else if(action==Exclusive::Setup)LinkPanel::openSetup();else if(action==Exclusive::Forget){DspiLink::forgetLinkNetwork();DspiLink::loadLinkSettings(LinkPanel::settings);enterPage(PAGE_LINK_NETWORK);}else if(action==Exclusive::Sd||action==Exclusive::Stop){recovery.request();enterPage(PAGE_MUSIC_ROOT);}else if(action!=Exclusive::None)enterPage(PAGE_MUSIC_ROOT);}
 if(recovery.pending()){
  const bool workersQuiet=s.core.stopped()&&s.browser.stopped()&&s.artwork.stopped()&&Services::storageQuiet();
  const bool outputQuiet=mediaPlayerPoc.serviceStopCleanup()&&mediaPlayerPoc.playbackState()==MediaPlaybackState::Stopped&&!mediaTrackTransitionActive();
  if(recovery.ready(workersQuiet,outputQuiet,!s.started&&!PanelWifi::snapshot().stationConnected)){
   if(!saveRecoveryPreferences())return;
   NetworkRecovery::BrowserReturn record{};if(returnToSd)NetworkRecovery::saveBrowser(record);memcpy(retainedBrowser,&record,sizeof record);
   AudioDiagnostics::event(AudioDiagnostics::Event::NetworkRestart,now);AudioDiagnostics::preserveForRestart();CoexistenceDiagnostics::preserveForRestart();showToast(returnToSd?"Restarting to SD":"Restarting panel",750);delay(750);ESP.restart();return;
  }
 }
}
