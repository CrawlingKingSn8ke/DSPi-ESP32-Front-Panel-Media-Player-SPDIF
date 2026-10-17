// Compiles the actual late sketch adapter against a fake native wire boundary.
// UI/prefs/media shims stay here; production menu/action/controller code is not
// copied or replaced by mocks. Every service pass must issue <= one frame.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include "../firmware/DSPi_ESP32_Front_Panel_v1_1_2/PanelControlMenus.h"
#include "../firmware/DSPi_ESP32_Front_Panel_v1_1_2/SpectrumRta.h"
#include "../firmware/DSPi_ESP32_Front_Panel_v1_1_2/UiReadability.h"
class String {
 public:std::string s;String(){}String(const char *v):s(v?v:""){}String(const std::string &v):s(v){}
  String(int v):s(std::to_string(v)){}String(unsigned v):s(std::to_string(v)){}String(float v,unsigned decimals=2){std::ostringstream o;o<<std::fixed<<std::setprecision(decimals)<<v;s=o.str();}
  String operator+(const String &v)const{return String(s+v.s);}bool operator==(const char *v)const{return s==v;}bool operator!=(const char *v)const{return s!=v;}const char *c_str()const{return s.c_str();}size_t length()const{return s.size();}
};
inline String operator+(const char *a,const String &b){return String(a)+b;}
enum MenuPage{PAGE_MAIN,PAGE_INPUT,PAGE_MEDIA,PAGE_PRESET,PAGE_LOUDNESS,PAGE_CROSSFEED,PAGE_LEVELLER,PAGE_PSYBASS,PAGE_BLUETOOTH,PAGE_SYSTEM,PAGE_MEDIA_SETTINGS,PAGE_SCREEN_SETTINGS,PAGE_IDLE_SCREEN,PAGE_SPECTRUM,PAGE_SPECTRUM_COLOURS,PAGE_THEME,PAGE_SUB_SYNTH,PAGE_SUB_SELECT,PAGE_SUB_OUTPUTS,PAGE_TUBE,PAGE_TUBE_OUTPUTS,PAGE_LIMITER,PAGE_LIMITER_OUTPUT,PAGE_FEATURES,
  PAGE_SPEAKERS,PAGE_SPEAKER_SETUP,PAGE_SPEAKER_GROUPS,PAGE_SPEAKER_MEMBERS,PAGE_OUTPUTS,PAGE_OUTPUT,PAGE_ROUTING,PAGE_ROUTE,PAGE_SUM,PAGE_SOURCE_PAIR,PAGE_EQUALISER,PAGE_EQ_INPUTS,PAGE_EQ_OUTPUTS,PAGE_EQ_TARGET,PAGE_EQ_BANDS,PAGE_EQ_BAND,PAGE_CROSSOVER,PAGE_XO_BAND,PAGE_PRESET_TOOLS,PAGE_PRESET_DESTINATION,PAGE_PRESET_PREVIEW,PAGE_STARTING_POINT,PAGE_INPUT_NAMES,PAGE_INPUT_NAME,PAGE_HARDWARE,PAGE_HARDWARE_OUTPUTS,PAGE_HARDWARE_PAIR,PAGE_PDM,PAGE_SPDIF_INPUTS,PAGE_SPDIF_INPUT,PAGE_I2S_INPUT,PAGE_I2S_DATA,PAGE_I2S_CLOCKS,PAGE_MASTER_CLOCK,PAGE_ADAT,PAGE_PANEL_CONFIRM,PAGE_PANEL_NAME,PAGE_PANEL_NUMBER,PAGE_COPY_DESTINATION,PAGE_COPY_PREVIEW,PAGE_COUNT};
#include "../firmware/DSPi_ESP32_Front_Panel_v1_1_2/PanelControlRuntime.h"
enum InputSource:uint8_t{SRC_USB,SRC_OPTICAL,SRC_I2S,SRC_ADAT,SRC_OPTICAL2,SRC_OPTICAL3,SRC_OPTICAL4,SRC_MAX=SRC_OPTICAL4};
enum {VIEW_HOME,VIEW_MENU,VIEW_EDIT,VIEW_BLE};
int uiView=VIEW_HOME;MenuPage menuPage=PAGE_MAIN;uint8_t menuIndex=0;bool editActive=false;
struct Dspi {bool connected=true,muted=false,spdifNonAudio=false;float sampleRate=48000;uint8_t activePreset=0,spdifState=0,adatState=0;InputSource source=SRC_USB;uint16_t occupiedPresets=1;char presetNames[10][32]{};}dspi;
struct Player {bool playing=false;bool active()const{return playing;}bool seeking()const{return false;}}mediaPlayerPoc;
bool mediaPlaybackSuspended=false,mediaRouteRestorePending=false,mediaPresetRefreshPending=false,presetSaveCompletionPending=false,dspiRequestInFlight=false;
char mediaCurrentPath[1]{};bool mediaTrackTransitionActive(){return false;}bool mediaPlaybackBufferLow(){return false;}bool wifiTransferExclusive(){return false;}void stopMediaPlayback(const char*){}
struct Preferences {std::map<std::string,std::vector<uint8_t>> data;bool fail=false;std::string failKey;
  size_t getBytesLength(const char *k){return data[k].size();}size_t getBytes(const char *k,void *out,size_t n){if(data[k].size()!=n)return 0;memcpy(out,data[k].data(),n);return n;}
  size_t putBytes(const char*k,const void*p,size_t n){if(fail||failKey==k)return 0;data[k]=std::vector<uint8_t>((const uint8_t*)p,(const uint8_t*)p+n);return n;}
}preferences;
enum {PREF_DIRTY_PANEL_CONTROLS=32,PRESET_PANEL_SETTINGS_VERSION=2,PRESET_SAVE_REFRESH_MS=250,REQ_GET_USER_MUTE=0xdd,REQ_SET_USER_MUTE=0xdc};
unsigned dirty=0;void markDeferredPreference(unsigned f){dirty|=f;}
struct PresetPanelSettingsRecord {uint8_t version,brightness,timeout,dim,timeoutAction,mainTextPalette,accentPalette,meterPalette,analogVu;};
uint8_t brightnessPercent=80,screenTimeoutOption=2,screenDimPercent=20,screenTimeoutAction=0,mainTextPaletteIndex=0,accentPaletteIndex=1,volumeMeterPaletteIndex=2,analogVuColourChoice=0;
SpectrumRta::SelectionRecord spectrumSelection;uint8_t spectrumBarColours[2][17]{},spectrumPeakColours[2][17]{};
void presetPanelSettingsKey(uint8_t s,char*k,size_t n){snprintf(k,n,"preset_ui%u",s);}void spectrumPresetKey(uint8_t s,char*k,size_t n){snprintf(k,n,"spectrum_p%u",s);}void spectrumBarPresetKey(uint8_t s,char*k,size_t n){snprintf(k,n,"spbar_p%u",s);}void spectrumPeakPresetKey(uint8_t s,char*k,size_t n){snprintf(k,n,"sppeak_p%u",s);}
bool ensurePresetMasterVolumeModeVerified(){return true;}bool savePresetPanelSettings(uint8_t){return true;}bool loadPreset(uint8_t,bool){return false;}
struct Font{}FontMedium;int16_t fontTextWidth(const Font&,const String &s){
#ifdef PANEL_TEST_MEASURE
  return PANEL_TEST_MEASURE(s.c_str());
#else
  return s.length()*11;
#endif
}
String presetText(){return String("P")+String(int(dspi.activePreset+1));}String inputSourceDisplayText(InputSource s){return String("Source ")+String(int(s));}String presetListLabel(uint8_t s){return String("P")+String(int(s+1));}bool inputSourceAvailable(InputSource s){return s<=SRC_MAX;}
String presetName(uint8_t){return "Original";} // Match the sketch's existing global API.
unsigned nowMs=1000;unsigned long millis(){return nowMs;}
String lastToast;void showToast(const String &s){lastToast=s;}void drawMenu(){}void enterPage(MenuPage p){menuPage=p;menuIndex=0;editActive=false;uiView=VIEW_MENU;panelEnterPage(p);}
enum DspiTxnResult{DSPI_TXN_OK,DSPI_TXN_ERROR,DSPI_TXN_TIMEOUT};
struct Native {
  struct Call{bool get;uint8_t req;uint16_t value,len;bool replay;uint8_t bytes[32];};
  std::vector<Call> calls;uint8_t pin=2,pinStatus=0,spdifMask=1,platform=1,adatPin=255,rejectEq=255;bool muted=false;unsigned saveCount=0;bool dropSaveAck=false,watchMono=false,unsafeMono=false,failPlatform=false,failRta=false,notifyDuringRead=false,notifyDuringNameRead=false,echoOwn=false;uint32_t selectedRate=48000;char channelName[32]="Output";
  PanelControls::BandRecipe eq[17][24]{};PanelControls::Route routes[8]{};float gains[9]{},delays[9]{};uint8_t mutes[9]{};uint16_t occupied=1;uint8_t active=0;char names[10][32]{};
  Native(){for(auto &t:eq)for(auto&b:t)b={1,0,1000,.707f,0,123};for(unsigned i=0;i<8;++i)routes[i]={uint8_t(i),0,i==0,true,-4};for(auto &g:gains)g=-12;strcpy(names[0],"Original");}
  bool deferredEqEcho=false;
  uint8_t deferredEqSource=0;bool alterDeferredEq=false;
  bool queueDeferredEq=false;std::vector<uint8_t> pendingEqEvent;
}nativeWire;
DspiTxnResult dspiTransactionResult(bool get,uint8_t req,uint16_t value,uint16_t,const uint8_t *tx,uint16_t txLen,uint8_t *rx,uint16_t,uint16_t &size,bool,bool replay){
  Native::Call call{};call.get=get;call.req=req;call.value=value;call.len=txLen;call.replay=replay;if(tx)memcpy(call.bytes,tx,txLen);nativeWire.calls.push_back(call);size=0;
  auto byte=[&](uint8_t v){rx[0]=v;size=1;};auto flt=[&](float v){PanelControls::putFloatLE(rx,v);size=4;};
  if(req==0x7e){memcpy(rx,"0123456789ABCDEF",16);size=16;}
  else if(req==0x7f){if(nativeWire.failPlatform)return DSPI_TXN_TIMEOUT;uint8_t b[]={nativeWire.platform,1,0x16,uint8_t(nativeWire.platform?9:5),1,6,1};memcpy(rx,b,7);size=7;}
  else if(req==0x0a){if(nativeWire.failRta)return DSPI_TXN_TIMEOUT;memset(rx,0,16);rx[0]=3;rx[1]=nativeWire.platform?8:2;rx[2]=nativeWire.platform?9:5;size=16;}
  else if(req==0x86){memset(rx,0,52);rx[0]=20;rx[2]=11;rx[3]=79;size=52;}
  else if(req==0x50){uint32_t n=2;memcpy(rx,&n,4);size=4;}
  else if(req==0xf6){uint8_t b[]={1,16,17,1,0,0,0,0};memcpy(rx,b,8);size=8;}
  else if(req==0x9c){memcpy(rx,nativeWire.channelName,32);size=32;}
  else if(req==0x9b){memcpy(nativeWire.channelName,tx,txLen);}
  else if(req==0x73)byte(0);else if(req==0x77)byte(nativeWire.mutes[value]);else if(req==0x75)flt(nativeWire.gains[value]);else if(req==0x79)flt(nativeWire.delays[value]);
  else if(req==0x76)nativeWire.mutes[value]=tx[0];else if(req==0x74)memcpy(&nativeWire.gains[value],tx,4);else if(req==0x78)memcpy(&nativeWire.delays[value],tx,4);
  else if(req==0x43){auto &b=nativeWire.eq[value>>8][(value>>3)&31];unsigned field=value&7;memset(rx,0,4);size=4;if(field==0)rx[0]=b.type;else if(field==4)rx[0]=b.bypass;else if(field==5){rx[0]=b.qpX512&255;rx[1]=b.qpX512>>8;}else PanelControls::putFloatLE(rx,field==1?b.frequencyHz:field==2?b.q:b.gainDb);}
  else if(req==0x42){if(tx[0]==nativeWire.rejectEq)return DSPI_TXN_ERROR;auto &b=nativeWire.eq[tx[0]][tx[1]];b.type=tx[2];b.bypass=tx[3];memcpy(&b.frequencyHz,tx+4,4);memcpy(&b.q,tx+8,4);memcpy(&b.gainDb,tx+12,4);if(txLen==18)b.qpX512=tx[16]|(uint16_t(tx[17])<<8);}
  else if(req==0x71){auto &r=nativeWire.routes[value>>8];rx[0]=r.input;rx[1]=r.output;rx[2]=r.enabled;rx[3]=r.invert;PanelControls::putFloatLE(rx+4,r.gainDb);size=8;}
  else if(req==0x70){auto &r=nativeWire.routes[tx[0]];r.enabled=tx[2];r.invert=tx[3];memcpy(&r.gainDb,tx+4,4);if(nativeWire.watchMono&&nativeWire.routes[0].enabled&&nativeWire.routes[1].enabled&&(nativeWire.routes[0].gainDb>-6||nativeWire.routes[1].gainDb>-6))nativeWire.unsafeMono=true;}
  else if(req==0x7d)byte(nativeWire.pin);else if(req==0x7c){if(!nativeWire.pinStatus)nativeWire.pin=value>>8;byte(nativeWire.pinStatus);}
  else if(req==0xdd)byte(nativeWire.muted);else if(req==0xdc)nativeWire.muted=tx[0];
  else if(req==0xef){uint8_t b[]={4,nativeWire.spdifMask,2,3,4,5};memcpy(rx,b,6);size=6;}
  else if(req==0xe9){uint8_t bit=1u<<(value>>8);if(value&1)nativeWire.spdifMask|=bit;else nativeWire.spdifMask&=~bit;byte(0);}
  else if(req==0xee){uint32_t current=44100;memcpy(rx,&current,4);memcpy(rx+4,&nativeWire.selectedRate,4);size=8;}
  else if(req==0xed){memcpy(&nativeWire.selectedRate,tx,4);}
  else if(req==0x6b)byte(nativeWire.adatPin);else if(req==0x6a){nativeWire.adatPin=value;byte(0);}
  else if(req==0x95){memset(rx,0,7);rx[0]=nativeWire.occupied&255;rx[1]=nativeWire.occupied>>8;rx[6]=1;size=7;}
  else if(req==0x9a)byte(nativeWire.active);
  else if(req==0x90){++nativeWire.saveCount;nativeWire.occupied|=1u<<value;nativeWire.active=value;byte(0);if(nativeWire.dropSaveAck)return DSPI_TXN_TIMEOUT;}
  else if(req==0x94){strncpy(nativeWire.names[value],(const char*)tx,31);}
  else if(req==0x93){memcpy(rx,nativeWire.names[value],32);size=32;}
  else if(get){memset(rx,0,txLen);size=txLen;}
  if(nativeWire.notifyDuringRead&&get&&req==0x43){nativeWire.notifyDuringRead=false;uint8_t event[13]={2,2,0,8,0,0,1,0,1,0,0,0,1};panelNotification(event,13);}
  if(nativeWire.notifyDuringNameRead&&get&&req==0x9c){nativeWire.notifyDuringNameRead=false;uint8_t event[13]={2,2,0,10,0,0,1,0,1,0,0,0,1};panelNotification(event,13);}
  if(nativeWire.echoOwn&&(!get||req==0x7c||req==0xe9)){uint8_t event[13]={2,2,0,9,0,0,1,0,8,0,0,0,1};panelNotification(event,13);}
  // DSPi applies EQ later in main.c after vendor dispatch reset the origin.
  if(nativeWire.deferredEqEcho&&!get&&req==0x42){
    const unsigned ch=tx[0],band=tx[1];unsigned offset=band<20?824+(ch*12+band)*16:4780+(ch*4+band-20)*16;
    uint8_t event[28]={2,2};event[4]=offset&255;event[5]=offset>>8;event[6]=16;
    event[8]=nativeWire.deferredEqSource;event[12]=tx[2];event[13]=tx[3];if(tx[2]==11&&txLen==18){event[14]=tx[16];event[15]=tx[17];}memcpy(event+16,tx+4,12);if(nativeWire.alterDeferredEq)event[16]^=1;
    if(nativeWire.queueDeferredEq)nativeWire.pendingEqEvent.assign(event,event+28);else panelNotification(event,28);
  }
  return DSPI_TXN_OK;
}
#define DSPI_HAVE_NIMBLE 0
#include "../firmware/DSPi_ESP32_Front_Panel_v1_1_2/PanelControlRuntime.inc"
static unsigned checks;
#define CHECK(x)do{++checks;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void pass(){unsigned before=nativeWire.calls.size();nowMs+=200;servicePanelControls();CHECK(nativeWire.calls.size()<=before+1);
 if(!nativeWire.pendingEqEvent.empty()){auto event=nativeWire.pendingEqEvent;nativeWire.pendingEqEvent.clear();panelNotification(event.data(),event.size());}}
static void passes(unsigned n){while(n--)pass();}
static void select(uint8_t row){menuIndex=row;CHECK(panelSelect());}
static void applyTestEdit(){panelApply();if(menuPage==PAGE_PANEL_NUMBER)select(2);}
int main(){
  strcpy(dspi.presetNames[0],"Original");passes(10);CHECK(nativeWire.calls.size()==1); // one boot identity read, then no Home polling
  enterPage(PAGE_SPEAKERS);passes(12);CHECK(PanelRuntime::model.map.inputs==8&&PanelRuntime::model.activeInputs==2);CHECK(panelMenuCount(PAGE_SPEAKERS)==4);
  strcpy(PanelRuntime::aliases.full[0],"TV");PanelRuntime::model.source=0;enterPage(PAGE_INPUT_NAME);select(0);CHECK(panelMenuCount(menuPage)==4);select(1);CHECK(editActive);for(unsigned i=0;i<4;++i)panelAdjust(1);applyTestEdit();CHECK(PanelRuntime::name.cursor==4);select(0);panelAdjust(1);applyTestEdit();select(2);CHECK(strcmp(PanelRuntime::aliases.full[0],"TV  A")==0);
  enterPage(PAGE_OUTPUTS);
  CHECK(PanelRuntime::model.roles.left==0);CHECK(menuPage==PAGE_OUTPUTS&&panelMenuCount(menuPage)==11);
  select(0);passes(10);select(3);CHECK(menuPage==PAGE_EQ_TARGET&&PanelRuntime::eqId()==8);select(0);select(0);passes(8);
  CHECK(menuPage==PAGE_EQ_BAND);menuIndex=2;select(2);CHECK(editActive);panelAdjust(1);applyTestEdit();passes(20);CHECK(nativeWire.eq[8][0].frequencyHz==1001&&nativeWire.eq[8][0].qpX512==123);
  enterPage(PAGE_ROUTING);select(2);CHECK(menuPage==PAGE_SUM);select(3);CHECK(menuPage==PAGE_PANEL_CONFIRM&&menuIndex==2);unsigned before=nativeWire.calls.size();select(2);passes(2);CHECK(nativeWire.calls.size()==before);
  enterPage(PAGE_SUM);select(3);select(3);nativeWire.watchMono=true;passes(170);nativeWire.watchMono=false;CHECK(nativeWire.routes[0].enabled&&nativeWire.routes[1].enabled);CHECK(fabsf(nativeWire.routes[0].gainDb+6.0206f)<.001f);CHECK(nativeWire.routes[0].invert);for(unsigned i=2;i<8;++i)CHECK(!nativeWire.routes[i].enabled);CHECK(!nativeWire.unsafeMono);
  enterPage(PAGE_HARDWARE_PAIR);passes(12);select(1);CHECK(editActive);panelAdjust(1);applyTestEdit();CHECK(menuPage==PAGE_PANEL_CONFIRM&&menuIndex==2);select(3);passes(20);CHECK(nativeWire.pin==3&&nativeWire.muted);CHECK(lastToast=="Requested - still muted");
  nativeWire.muted=false;PanelRuntime::model.source=1;enterPage(PAGE_SPDIF_INPUT);passes(8);CHECK(panelMenuValue(menuPage,0)=="Off");select(0);panelAdjust(1);applyTestEdit();select(3);passes(25);CHECK(nativeWire.spdifMask==3&&!nativeWire.muted);CHECK(panelMenuValue(PAGE_SPDIF_INPUT,0)=="On");
  enterPage(PAGE_I2S_CLOCKS);menuIndex=5;passes(6);select(5);CHECK(editActive);panelAdjust(1);CHECK(PanelRuntime::localValue==96000);applyTestEdit();select(3);passes(25);CHECK(nativeWire.selectedRate==96000&&nativeWire.muted);CHECK(lastToast=="Selected rate - still muted");
  nativeWire.muted=false;enterPage(PAGE_PRESET_TOOLS);select(1);passes(2);select(1);CHECK(menuPage==PAGE_PANEL_NAME);select(2);CHECK(menuPage==PAGE_PRESET_PREVIEW);select(6);CHECK(menuIndex==2);brightnessPercent=40;select(3);brightnessPercent=90;passes(35);
  CHECK(nativeWire.saveCount==1);CHECK(nativeWire.occupied==3&&nativeWire.active==1);CHECK(preferences.data["preset_ui1"][1]==40);CHECK(lastToast=="Save accepted");
  const char *filterLabels[]={"Off","Peaking","Low Shelf","High Shelf","Low Pass","High Pass","Notch","All Pass","All Pass 1","Low Shelf 1","High Shelf 1","Linkwitz","Low Pass 1","High Pass 1"};for(unsigned t=0;t<14;++t)CHECK(strcmp(PanelRuntime::filterName(t),filterLabels[t])==0);
  PanelRuntime::groupJob=1;PanelRuntime::groupMask=3;PanelRuntime::groupAt=1;
  uint8_t presetEvent[8]={2,4,0,1,2,0,0,0};panelNotification(presetEvent,8);CHECK(PanelRuntime::groupJob==0);before=nativeWire.calls.size();passes(8);for(unsigned i=before;i<nativeWire.calls.size();++i)CHECK(nativeWire.calls[i].req!=0x76);
  uint8_t ownEvent[13]={2,2,0,2,0,0,1,0,8,0,0,0,1};uint32_t gen=PanelRuntime::generation;panelNotification(ownEvent,13);CHECK(PanelRuntime::generation==gen);ownEvent[8]=1;panelNotification(ownEvent,13);CHECK(PanelRuntime::generation==gen+1);
  enterPage(PAGE_PRESET_TOOLS);select(1);passes(2);select(3);select(2);select(6);select(3);CHECK(PanelRuntime::presetStage==1);CHECK(panelBack());CHECK(PanelRuntime::presetStage==0);
  PanelRuntime::model.output=0;enterPage(PAGE_OUTPUT);menuIndex=5;passes(8);select(5);CHECK(menuPage==PAGE_PANEL_NAME);strcpy(PanelRuntime::name.text,"Changed");select(2);pass();CHECK(PanelRuntime::nameStage==2);CHECK(panelBack());CHECK(PanelRuntime::nameStage==0);
  PanelRuntime::model.eqOutput=true;PanelRuntime::model.target=0;PanelRuntime::model.band=0;nativeWire.eq[8][20]={0,0,1000,.707f,0,0};enterPage(PAGE_XO_BAND);passes(8);select(0);CHECK(PanelRuntime::localValue==0);applyTestEdit();passes(18);CHECK(nativeWire.eq[8][20].type==0);
  select(0);panelAdjust(1);applyTestEdit();passes(18);CHECK(nativeWire.eq[8][20].type==32&&nativeWire.eq[8][20].bypass==0);
  dspi.activePreset=0;PanelRuntime::roleSlot=255;panelPresetChanged(0);PanelRuntime::model.group=0;enterPage(PAGE_SPEAKER_MEMBERS);select(0);CHECK(PanelRuntime::model.roles.left==1);
  char p1RoleKey[16];PanelRuntime::localKey("r",0,p1RoleKey,sizeof(p1RoleKey));dspi.activePreset=1;panelPresetChanged(1);CHECK(panelPersistLocal());CHECK(preferences.getBytesLength(p1RoleKey)==sizeof(PanelRuntime::RoleRecord));PanelRuntime::RoleRecord storedRole{};preferences.getBytes(p1RoleKey,&storedRole,sizeof(storedRole));CHECK(storedRole.roles.left==1);
  PanelRuntime::model.roles.left=1;PanelRuntime::model.roles.right=2;nativeWire.gains[0]=-10;nativeWire.gains[1]=-14;nativeWire.dropSaveAck=true;nativeWire.muted=false;
  enterPage(PAGE_PRESET_TOOLS);select(1);passes(2);select(2);select(2);select(6);brightnessPercent=35;select(3);brightnessPercent=95;before=nativeWire.calls.size();passes(60);nativeWire.dropSaveAck=false;
  CHECK(nativeWire.saveCount==2);bool resolvedDir=false,resolvedActive=false,sawSave=false;for(unsigned i=before;i<nativeWire.calls.size();++i){const auto&f=nativeWire.calls[i];if(f.req==0x90){CHECK(!sawSave);sawSave=true;}else if(sawSave&&f.req==0x95)resolvedDir=true;else if(sawSave&&f.req==0x9a)resolvedActive=true;}CHECK(resolvedDir&&resolvedActive);CHECK(preferences.data["preset_ui2"][1]==35);
  CHECK(PanelRuntime::savedKnown&&PanelRuntime::savedSlot==2&&PanelRuntime::savedGains[0]==-10&&PanelRuntime::savedGains[1]==-14);
  PanelRuntime::model.output=0;enterPage(PAGE_OUTPUT);passes(8);select(1);panelAdjust(1);applyTestEdit();passes(8);CHECK(nativeWire.gains[0]==-9.f&&PanelRuntime::savedKnown);
  enterPage(PAGE_SPEAKER_SETUP);passes(5);dspi.activePreset=1;CHECK(panelMenuValue(PAGE_SPEAKER_SETUP,1)=="Unavailable");dspi.activePreset=2;PanelRuntime::savedSerial[0]='X';CHECK(panelMenuValue(PAGE_SPEAKER_SETUP,1)=="Unavailable");PanelRuntime::savedSerial[0]='0';PanelRuntime::subBaseline.valid=PanelRuntime::balanceBaseline.valid=true;select(1);select(3);passes(45);CHECK(nativeWire.gains[0]==-10&&nativeWire.gains[1]==-14);CHECK(!PanelRuntime::subBaseline.valid&&!PanelRuntime::balanceBaseline.valid);
  panelPresetLoaded(2);nativeWire.gains[0]=-2;enterPage(PAGE_SPEAKER_SETUP);before=nativeWire.calls.size();passes(15);CHECK(!PanelRuntime::savedKnown&&panelMenuValue(PAGE_SPEAKER_SETUP,1)=="Unavailable");for(unsigned i=before;i<nativeWire.calls.size();++i)CHECK(nativeWire.calls[i].req!=0x75);
  dspi.activePreset=0;PanelRuntime::roleSlot=0;PanelRuntime::model.roles.left=4;panelPresetLoaded(0);CHECK(PanelRuntime::model.roles.left==1);
  enterPage(PAGE_ADAT);passes(8);select(1);CHECK(editActive);before=nativeWire.calls.size();applyTestEdit();passes(3);CHECK(menuPage==PAGE_ADAT&&nativeWire.calls.size()==before);select(1);panelAdjust(1);CHECK(PanelRuntime::originalValue==255&&PanelRuntime::localValue==0);applyTestEdit();select(3);passes(25);CHECK(nativeWire.adatPin==0);
  PanelRuntime::model.output=0;strcpy(nativeWire.channelName,"Sub_1");PanelRuntime::namesKnown[8]=false;enterPage(PAGE_OUTPUT);menuIndex=5;passes(10);select(5);CHECK(menuPage==PAGE_OUTPUT&&!editActive);strcpy(nativeWire.channelName,"Output");PanelRuntime::namesKnown[8]=false;
  PanelRuntime::model.eqOutput=true;PanelRuntime::model.target=0;PanelRuntime::model.band=0;PanelRuntime::model.link=true;nativeWire.eq[8][0]={1,0,1000,.707f,0,123};nativeWire.eq[9][0]={1,0,1000,3.5f,-3,444};nativeWire.rejectEq=9;nativeWire.echoOwn=true;enterPage(PAGE_EQ_BAND);passes(8);select(2);panelAdjust(1);gen=PanelRuntime::generation;applyTestEdit();passes(35);CHECK(PanelRuntime::generation==gen);CHECK(nativeWire.eq[8][0].frequencyHz==1001&&nativeWire.eq[9][0].frequencyHz==1000&&nativeWire.eq[9][0].q==3.5f);CHECK(lastToast=="Partial - verify state");nativeWire.rejectEq=255;nativeWire.echoOwn=false;
  enterPage(PAGE_EQ_BAND);passes(8);select(2);panelAdjust(1);applyTestEdit();before=nativeWire.calls.size();gen=PanelRuntime::generation;nativeWire.notifyDuringRead=true;passes(20);CHECK(PanelRuntime::generation==gen+1);for(unsigned i=before;i<nativeWire.calls.size();++i)CHECK(nativeWire.calls[i].req!=0x42);
  // Real outer daily job stops between members when a new preset arrives.
  PanelRuntime::model.roles.left=1;PanelRuntime::model.roles.right=2;nativeWire.mutes[0]=nativeWire.mutes[1]=0;enterPage(PAGE_SPEAKERS);passes(8);select(0);for(unsigned i=0;i<12&&!(PanelRuntime::groupAt==1&&!PanelRuntime::control.busy());++i)pass();CHECK(PanelRuntime::groupAt==1&&nativeWire.mutes[0]==1&&nativeWire.mutes[1]==0);panelNotification(presetEvent,8);before=nativeWire.calls.size();passes(12);CHECK(nativeWire.mutes[1]==0);for(unsigned i=before;i<nativeWire.calls.size();++i)CHECK(nativeWire.calls[i].req!=0x76);
  // The first linked target may be applied; no SET reaches its next member.
  PanelRuntime::model.link=true;enterPage(PAGE_EQ_BAND);passes(8);select(2);panelAdjust(1);applyTestEdit();for(unsigned i=0;i<30&&PanelRuntime::confirmedTargets!=1;++i)pass();CHECK(PanelRuntime::confirmedTargets==1&&PanelRuntime::control.busy());ownEvent[8]=1;panelNotification(ownEvent,13);before=nativeWire.calls.size();passes(15);for(unsigned i=before;i<nativeWire.calls.size();++i)CHECK(nativeWire.calls[i].req!=0x42);
  // Hardware stages are busy even when the scalar controller is idle.
  nativeWire.muted=false;enterPage(PAGE_HARDWARE_PAIR);passes(8);select(1);panelAdjust(1);applyTestEdit();select(3);CHECK(PanelRuntime::hardwareGuard==1);float expectedPin=PanelRuntime::localValue;select(1);CHECK(!editActive&&PanelRuntime::localValue==expectedPin);CHECK(panelBack());before=nativeWire.calls.size();passes(5);for(unsigned i=before;i<nativeWire.calls.size();++i)CHECK(nativeWire.calls[i].req!=0x7c);
  enterPage(PAGE_HARDWARE_PAIR);passes(8);select(1);panelAdjust(1);applyTestEdit();select(3);for(unsigned i=0;i<20&&PanelRuntime::hardwareGuard!=6;++i)pass();CHECK(PanelRuntime::hardwareGuard==6);expectedPin=PanelRuntime::localValue;select(1);CHECK(!editActive&&PanelRuntime::localValue==expectedPin&&PanelRuntime::hardwareGuard==6);CHECK(panelBack());passes(8);CHECK(PanelRuntime::hardwareGuard==0&&nativeWire.muted);
  PanelRuntime::model.output=0;enterPage(PAGE_OUTPUT);menuIndex=5;passes(10);select(5);CHECK(menuPage==PAGE_PANEL_NAME);strcpy(PanelRuntime::name.text,"Changed");select(2);nativeWire.notifyDuringNameRead=true;pass();CHECK(PanelRuntime::nameStage==0);before=nativeWire.calls.size();passes(8);for(unsigned i=before;i<nativeWire.calls.size();++i)CHECK(nativeWire.calls[i].req!=0x9b);
  PanelRuntime::model.eqOutput=true;PanelRuntime::model.target=0;PanelRuntime::model.band=0;nativeWire.eq[8][0].q=.707f;enterPage(PAGE_EQ_BAND);passes(8);select(4);CHECK(panelMenuValue(menuPage,0)=="00.71");panelNameMove(1);panelNameMove(1);panelAdjust(1);CHECK(panelMenuValue(menuPage,0)=="00.72");panelBack();
  nativeWire.eq[8][20]={32,0,1000,.707f,0,0};enterPage(PAGE_XO_BAND);passes(8);select(3);CHECK(panelMenuValue(menuPage,3)=="12");for(unsigned order=4;order<=8;order+=2){panelAdjust(1);CHECK(panelMenuValue(menuPage,3)==String(int(order*6)).c_str());}panelCancel();select(1);CHECK(panelMenuValue(menuPage,1)=="Low Pass");panelAdjust(1);CHECK(panelMenuValue(menuPage,1)=="High Pass");panelCancel();select(2);CHECK(panelMenuValue(menuPage,2)=="Linkwitz");panelAdjust(1);CHECK(panelMenuValue(menuPage,2)=="Butterworth");panelAdjust(1);CHECK(panelMenuValue(menuPage,2)=="Bessel");panelCancel();
  dspi.connected=false;pass();nativeWire.platform=0;nativeWire.failPlatform=nativeWire.failRta=true;dspi.connected=true;enterPage(PAGE_OUTPUTS);passes(10);CHECK(!PanelRuntime::model.map.writable&&!PanelRuntime::control.map.writable);CHECK(!PanelRuntime::evidence.countsConfirmed);
  dspi.connected=false;pass();nativeWire.failPlatform=nativeWire.failRta=false;dspi.connected=true;enterPage(PAGE_ADAT);passes(10);CHECK(PanelRuntime::model.map.inputs==2&&PanelRuntime::model.map.outputs==5);before=nativeWire.calls.size();select(0);CHECK(!editActive);passes(4);for(unsigned i=before;i<nativeWire.calls.size();++i)CHECK(nativeWire.calls[i].get);
  for(const auto &f:nativeWire.calls){CHECK(f.req!=0xa0&&f.req!=0xa1&&f.req!=0x53&&f.req!=0x91);if(!f.get||f.req==0x90||f.req==0x7c)CHECK(!f.replay);}
  printf("PanelRuntime: %u checks passed\n",checks);
  return 0;
}
