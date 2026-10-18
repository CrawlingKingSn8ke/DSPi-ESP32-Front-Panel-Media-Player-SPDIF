// Included last: deliberately no new UART owner and no player implementation.
namespace PanelRuntime {
using PanelControlMenus::Page;
using PanelControlMenus::ValueKind;
using PanelControlMenus::Frame;
using PanelControlMenus::Reply;
using PanelControlMenus::WireStatus;
using PanelControls::Completion;
static Page page(MenuPage p){return static_cast<Page>(int(p)-int(PAGE_SPEAKERS)+int(Page::Speakers));}
static MenuPage native(Page p){
  switch(p){case Page::Main:return PAGE_MAIN;case Page::Preset:return PAGE_PRESET;case Page::Effects:return PAGE_FEATURES;case Page::System:return PAGE_SYSTEM;default:return static_cast<MenuPage>(int(p)-int(Page::Speakers)+int(PAGE_SPEAKERS));}
}
static bool wireActive=false;
static bool pendingInvalidation=false,pendingRoleReload=false,externalChangeNotice=false;
static void drainInvalidation();
// EQ is applied asynchronously in DSPi main.c, after dispatch resets the
// notification origin to UNKNOWN. Correlate only exact, recent band echoes;
// never treat all notifications arriving during a transaction as our own.
struct EqEcho {bool valid=false;uint16_t offset=0;uint32_t sent=0;uint8_t bytes[16]{};};
static EqEcho eqEchoes[4];
static uint8_t eqEchoNext=0;
static int rememberEqWrite(const Frame &f){
  if(f.get||f.request!=0x42||(f.length!=16&&f.length!=18)||f.bytes[0]>=17)return -1;
  const uint8_t band=f.bytes[1];if(band>=10&&(band<20||band>23))return -1;
  const uint8_t slot=eqEchoNext++%4;EqEcho &e=eqEchoes[slot];e=EqEcho();e.valid=true;e.sent=millis();
  // Pinned unified WireBulkParams: eq @824, 12 bands/channel; XO @4780,
  // four slots/channel. Each WireBandParams is 16 bytes on both platforms.
  e.offset=band<10?824+(f.bytes[0]*12+band)*16:4780+(f.bytes[0]*4+band-20)*16;
  e.bytes[0]=f.bytes[2];e.bytes[1]=f.bytes[3]==1?1:0;
  if(f.bytes[2]==11&&f.length==18){e.bytes[2]=f.bytes[16];e.bytes[3]=f.bytes[17];}
  memcpy(e.bytes+4,f.bytes+4,12);return slot;
}
static bool consumeEqEcho(const uint8_t *p,uint16_t n){
  if(!p||n!=28||p[0]!=2||p[1]!=2||p[2]||p[6]!=16||p[7]||p[8]!=0||p[9]||p[10]||p[11])return false;
  const uint16_t offset=p[4]|(uint16_t(p[5])<<8);
  for(auto &e:eqEchoes){if(e.valid&&uint32_t(millis()-e.sent)>2000)e.valid=false;
    if(e.valid&&e.offset==offset&&memcmp(e.bytes,p+12,16)==0){e.valid=false;return true;}}
  return false;
}
static void clearEqEchoes(){for(auto &e:eqEchoes)e.valid=false;}
static Reply exchange(void *,const Frame &f){
  Reply r;uint16_t size=0;wireActive=true;
  const int echo=rememberEqWrite(f); // Register before UART can deliver the event.
  DspiTxnResult status=dspiTransactionResult(f.get,f.request,f.value,2,
      f.get?nullptr:f.bytes,f.length,r.bytes,sizeof(r.bytes),size,false,
      f.get&&!f.action);
  wireActive=false;
  if(status==DSPI_TXN_ERROR&&echo>=0)eqEchoes[echo].valid=false;
  r.size=size;
  r.status=status==DSPI_TXN_OK?WireStatus::Ok:
      status==DSPI_TXN_ERROR?WireStatus::Rejected:WireStatus::Unknown;
  if(pendingInvalidation){drainInvalidation();r.status=WireStatus::Unknown;}
  return r;
}
static PanelControlMenus::Model model;
static PanelControlMenus::Controller control(exchange,nullptr);
static PanelControlMenus::NameEditor name;
struct Cell {float value=0;bool known=false;bool attempted=false;};
static Cell cells[12];
static uint16_t cachePage=0xffff;
static uint8_t cacheFirst=255,readField=0;
static uint32_t generation=0,editGeneration=0;
static bool capabilityAttempted=false,identityAttempted=false,serialKnown=false;
static uint8_t capabilityStep=0,platformByte=0,reportedOutputs=0;
static PanelControls::CapabilityEvidence evidence{};
static char serial[17]{};
static char channelNames[17][32]{};
static bool namesKnown[17]{};
static bool namesEditable[17]{};
struct Aliases {uint8_t version=1;char serial[17]{};char full[8][32]{};char shortName[8][32]{};};
static Aliases aliases;
struct InputMenuRecord {uint8_t version=1;char serial[17]{};PanelInputMenu menu;};
static PanelInputMenu inputMenu;
static bool inputMenuDirty=false;
struct RoleRecord {uint8_t version=1;char serial[17]{};PanelControlMenus::Roles roles;};
struct RoleDraft {RoleRecord record;bool dirty=false;};
static RoleDraft roleDrafts[10];
static RoleRecord legacySaveRoles;
static uint8_t legacySaveSlot=255;
static bool legacySaveRolesKnown=false;
static uint8_t roleSlot=255;
static bool rolesDirty=false,aliasesDirty=false;
static float localValue=0,originalValue=0;
static PanelNumberEditor number;
static MenuPage numberParent=PAGE_EQ_BAND;
static uint8_t numberRow=0;
static const char *numberUnit="";
static const char *statusText="";
static MenuPage editPage=PAGE_SPEAKERS;
static uint8_t editRow=0;
static bool awaitingEdit=false;
static bool lastControlBusy=false;
static uint8_t activeInputStep=0;
static bool uartAttempted=false,operationCancelled=false,presetCancelled=false;
static uint8_t linkedTarget=255;
static uint8_t linkStage=0,linkedBand=0,linkedSource=0,linkType=0;
static uint32_t linkGeneration=0;
static bool linkedCopy=false,linkedSecond=false,linkedXoEnable=false;
static PanelControls::BandField linkedField=PanelControls::BandField::Type;
static float linkedValue=0;
static uint8_t confirmedTargets=0;
enum class ConfirmAction:uint8_t {None,Hardware,Restore,Save,SaveNew,New,Rename,Revert,Sum,LinkBand,CopyParameters};
static ConfirmAction confirmation=ConfirmAction::None;
static ConfirmAction presetJobAction=ConfirmAction::None;
static uint8_t presetJobSlot=0,presetActiveBefore=0,captureOutput=0;
static char presetJobName[32]{};
static uint32_t presetJobGeneration=0,hardwareGeneration=0;
static bool saveAttempted=false,saveAckKnown=false,presetWasEmpty=false;
static MenuPage returnPage=PAGE_SPEAKERS;
static uint8_t returnRow=0;
static uint8_t presetTool=0,presetSlot=0;
static bool effectsOff=false;
static char presetNameDraft[32]{};
static uint8_t namePurpose=0; // 0 alias, 1 short alias, 2 channel, 3 preset
static uint8_t nameStage=0;
static uint8_t nameJobTarget=0;static char nameJobValue[32]{};static bool nameReturnAllowed=true,renameAttempted=false;
static uint8_t presetStage=0,cleanEffect=0,cleanInput=0,cleanBand=0;
static uint16_t occupiedMask=0;
static bool directoryKnown=false;
static unsigned long settleUntil=0;
static uint8_t uartTx=16,uartRx=17;
static bool uartKnown=false;
static bool hardwareAction=false;
static uint8_t hardwareGuard=0;
static bool hardwarePriorMute=true,hardwareLeaveMuted=false,hardwareCancel=false;
static uint8_t hardwareReadbacks=0;
static bool roleCaptureMute=false;
static uint8_t groupJob=0,groupAt=0; // mute, gain capture/apply, restore, sum
static uint16_t groupMask=0;
static float freshGains[9]{},plannedGains[9]{};
static uint16_t gainKnownMask=0;
static uint16_t gainAttemptedMask=0;
static uint8_t freshMutes[9]{},savedMutes[9]{};
static float savedGains[9]{};
static uint16_t savedMask=0;
static uint8_t savedSlot=255;
static char savedSerial[17]{};
static bool savedKnown=false,savedEligible=false;
static bool savedContext(){return savedKnown&&serialKnown&&savedSlot==dspi.activePreset&&memcmp(savedSerial,serial,17)==0;}
static PanelControls::GainBaseline gainBaseline{},subBaseline{},balanceBaseline{};
static float balance=0,subTrim=0;
static uint8_t sourcePair=0;
static PanelControls::Route sumRoutes[8]{};
static uint8_t sumStage=0;
static PanelControls::Route sumOriginal[2]{};
static float sumGains[2]{};
static uint8_t sumKnown=0,sumAttempted=0,sumOutput=0,sumPair=0;
static uint32_t sumGeneration=0;
static bool sumContext(){return sumGeneration==generation&&sumOutput==model.output&&sumPair==sourcePair&&sumPair*2+1<model.visibleInputs()&&model.map.writable;}
static bool sumReady(){return sumKnown==3&&sumContext();}
static String gainText(float value){
  long ticks=lroundf(value*100),m=ticks<0?-ticks:ticks;char text[20];
  snprintf(text,sizeof(text),"%s%02ld.%02ld",ticks<0?"-":"",m/100,m%100);return String(text);
}
struct CopyParametersJob {
  uint8_t source=0,target=0,stage=0,read=0,write=0,mute=0;
  uint32_t generation=0;
  PanelControls::BandRecipe bands[14]{};
  float gain=0,delay=0;
};
static CopyParametersJob copyJob;
static uint8_t copyNativeBand(uint8_t n){return n<10?n:20+n-10;}
struct PresetSnapshot {
  PresetPanelSettingsRecord ui{};
  decltype(spectrumSelection) spectrum;
  uint8_t bars[sizeof(spectrumBarColours)]{},peaks[sizeof(spectrumPeakColours)]{};
  PanelControlMenus::Roles roles;
  char serial[17]{};
  float gains[9]{};uint8_t mutes[9]{};uint16_t speakerMask=0;
  bool speakerKnown=false;
  bool valid=false;
};
static PresetSnapshot presetSnapshot;
static void localKey(const char *,uint8_t,char *,size_t);
static void serialKey(const char *,const char *,uint8_t,char *,size_t);
static void capturePresetSnapshot(){
  presetSnapshot.ui={PRESET_PANEL_SETTINGS_VERSION,brightnessPercent,screenTimeoutOption,screenDimPercent,
    uint8_t(screenTimeoutAction),mainTextPaletteIndex,accentPaletteIndex,volumeMeterPaletteIndex,uint8_t(analogVuColourChoice)};
  presetSnapshot.spectrum=spectrumSelection;memcpy(presetSnapshot.bars,&spectrumBarColours,sizeof(spectrumBarColours));memcpy(presetSnapshot.peaks,&spectrumPeakColours,sizeof(spectrumPeakColours));presetSnapshot.roles=model.roles;presetSnapshot.valid=true;
  memcpy(presetSnapshot.serial,serial,17);presetSnapshot.speakerMask=model.roles.left|model.roles.right|model.roles.subs;
  presetSnapshot.speakerKnown=model.map.writable&&presetSnapshot.speakerMask&&!(presetSnapshot.speakerMask>>model.map.outputs);captureOutput=0;
}
static bool persistPresetSnapshot(uint8_t slot){
  if(!presetSnapshot.valid||!serialKnown||memcmp(presetSnapshot.serial,serial,17))return false;char key[16];bool ok=true;
  presetPanelSettingsKey(slot,key,sizeof(key));ok&=preferences.putBytes(key,&presetSnapshot.ui,sizeof(presetSnapshot.ui))==sizeof(presetSnapshot.ui);
  spectrumPresetKey(slot,key,sizeof(key));ok&=preferences.putBytes(key,&presetSnapshot.spectrum,sizeof(presetSnapshot.spectrum))==sizeof(presetSnapshot.spectrum);
  spectrumBarPresetKey(slot,key,sizeof(key));ok&=preferences.putBytes(key,presetSnapshot.bars,sizeof(presetSnapshot.bars))==sizeof(presetSnapshot.bars);
  spectrumPeakPresetKey(slot,key,sizeof(key));ok&=preferences.putBytes(key,presetSnapshot.peaks,sizeof(presetSnapshot.peaks))==sizeof(presetSnapshot.peaks);
  RoleRecord role;memcpy(role.serial,serial,17);role.roles=presetSnapshot.roles;localKey("r",slot,key,sizeof(key));ok&=preferences.putBytes(key,&role,sizeof(role))==sizeof(role);
  return ok;
}

struct Field {
  const char *label="";const char *unit="";uint8_t get=0,set=0;uint16_t selector=0;
  ValueKind kind=ValueKind::Byte;float min=0,max=1,step=1;
  bool hardware=false,action=false,pin=false,readonly=false;
};
static Field scalar(const char *label,uint8_t get,uint8_t set,uint16_t sel,ValueKind kind,
                    float low,float high,float step,const char *unit=""){
  Field f;f.label=label;f.get=get;f.set=set;f.selector=sel;f.kind=kind;f.min=low;f.max=high;f.step=step;f.unit=unit;return f;
}
static Field hardware(const char *label,uint8_t get,uint8_t set,uint16_t sel,
                      float low,float high,float step=1,bool pin=false,bool action=true){
  Field f=scalar(label,get,set,sel,ValueKind::Byte,low,high,step,pin?"GPIO":"");
  f.hardware=true;f.action=action;f.pin=pin;return f;
}
static uint8_t eqId(){return model.eqOutput?model.map.inputs+model.target:model.target;}
static uint8_t eqBand(){return menuPage==PAGE_XO_BAND?20+model.band:model.band;}
static Field field(MenuPage p,uint8_t row){
  const uint8_t out=model.output,pair=model.pair;
  if((p==PAGE_OUTPUTS&&row<model.map.outputs)||p==PAGE_SPEAKER_MEMBERS||p==PAGE_EQ_OUTPUTS)return scalar("",0x73,0,row,ValueKind::Byte,0,1,1);
  if(p==PAGE_OUTPUT){switch(row){case 6:{Field f=scalar("Enabled",0x73,0x72,out,ValueKind::Byte,0,1,1);f.hardware=true;return f;}
    case 0:return scalar("Mute",0x77,0x76,out,ValueKind::Byte,0,1,1);
    case 1:return scalar("Gain",0x75,0x74,out,ValueKind::Float,-60,0,0.1f,"dB");
    case 2:return scalar("Delay",0x79,0x78,out,ValueKind::Float,0,PanelControlMenus::maxDelayMs(model.map,out,control.sampleRate),0.1f,"ms");default:break;}}
  if(p==PAGE_ROUTE){Field f=scalar(row==0?"Enabled":row==1?"Gain":"Invert",0x71,0x70,(uint16_t(model.input)<<8)|out,
      row==1?ValueKind::Float:ValueKind::Byte,row==1?-60:0,row==1?0:1,row==1?0.1f:1,row==1?"dB":"");return f;}
  if(p==PAGE_HARDWARE_PAIR){if(row==0){Field f=hardware("Format",0xc1,0,pair,0,1);f.readonly=true;return f;}return hardware("Data Pin",0x7d,0x7c,pair,0,29,1,true);}
  if(p==PAGE_PDM)return hardware("Data Pin",0x7d,0x7c,model.map.stereoPairs,0,29,1,true);
  if(p==PAGE_SPDIF_INPUT)return row==0?hardware("Enabled",0xef,model.source?0xe9:0,model.source,0,1):hardware("Data Pin",0xe5,0xe4,model.source,0,29,1,true);
  if(p==PAGE_I2S_INPUT&&row==0)return hardware("Channels",0xf4,0xf3,0,2,model.map.inputs,2);
  if(p==PAGE_I2S_DATA)return hardware("Data Pin",0xf2,0xf1,row,0,29,1,true);
  if(p==PAGE_I2S_CLOCKS){switch(row){case 0:return hardware("Clock Mode",0x89,0x88,0,0,1,1,false,false);
    case 1:return hardware("Clock Pins",0xff,0xfe,0,0,1);
    case 2:return hardware("Master BCLK",0xc3,0xc2,0,0,28,1,true);
    case 3:return hardware("Slave BCLK",0xc3,0xc2,1,0,28,1,true);
    case 4:{Field f=hardware("LRCLK",0xc3,0,0,0,29,1,true);f.readonly=true;return f;}
    default:{Field f=hardware("Sample Rate",0xee,0xed,0,44100,96000);f.action=false;f.kind=ValueKind::Dword;f.unit="Hz";return f;}}}
  if(p==PAGE_MASTER_CLOCK){switch(row){case 0:return hardware("Enabled",0xc5,0xc4,0,0,1);
    case 1:return hardware("Data Pin",0xc7,0xc6,0,0,29,1,true);default:return hardware("Multiplier",0xc9,0xc8,0,0,1);}}
  if(p==PAGE_ADAT){switch(row){case 0:return hardware("Input Enabled",0x69,0x68,0,0,1);
    case 1:return hardware("Input Pin",0x6b,0x6a,0,0,29,1,true);
    case 2:return hardware("Input Clock",0x6d,0x6c,0,0,1);
    case 3:return hardware("Output Enabled",0xcb,0xca,0,0,1);
    case 4:return hardware("Output Pin",0xcd,0xcc,0,0,29,1,true);
    case 5:{Field f=hardware("Input State",0x6e,0,0,0,1);f.readonly=true;return f;}
    default:{Field f=hardware("Output State",0xce,0,0,0,1);f.readonly=true;return f;}}}
  if(p==PAGE_HARDWARE&&row==7)return hardware("Preset Mode",0x99,0x98,0,0,1,1,false,false);
  return Field();
}
static Reply query(uint8_t request,uint16_t selector,uint16_t length){Frame f;f.request=request;f.value=selector;f.length=length;return exchange(nullptr,f);}
static void clearCells(){for(auto &c:cells)c=Cell();readField=0;cacheFirst=255;}
static bool validReply(const Reply &r,unsigned n){return r.status==WireStatus::Ok&&r.size==n;}
static bool readCell(MenuPage p,uint8_t row){
  Field f=field(p,row);if(!f.get){cells[row].attempted=true;return false;}
  uint16_t length=f.kind==ValueKind::Float?4:f.kind==ValueKind::Word?2:1;
  if(f.get==0x71)length=8;if(f.get==0xef)length=6;if(f.get==0xee)length=8;
  if(f.get==0x6e)length=20;if(f.get==0xce)length=8;
  Reply r=query(f.get,f.get==0xef?0:f.selector,length);cells[row].attempted=true;
  if(!validReply(r,length))return true;
  float v=f.kind==ValueKind::Float?PanelControlMenus::wireFloat(r.bytes):f.kind==ValueKind::Word?float(r.bytes[0]|(uint16_t(r.bytes[1])<<8)):float(r.bytes[0]);
  if(f.get==0x71)v=row==0?r.bytes[2]:row==1?PanelControlMenus::wireFloat(r.bytes+4):r.bytes[3];
  if(f.get==0xef)v=(r.bytes[1]>>model.source)&1;
  if(f.get==0xee){uint32_t hz;memcpy(&hz,r.bytes+4,4);v=hz;}
  if(p==PAGE_I2S_CLOCKS&&row==4)v+=1;
  cells[row].value=v;cells[row].known=PanelControls::finite(v);return true;
}
static void serialKey(const char *identity,const char *kind,uint8_t slot,char *out,size_t n){
  // Serial is also stored and checked in the value: a key hash collision can
  // never apply another device's aliases or speaker roles.
  uint32_t hash=2166136261u;for(unsigned i=0;i<16;++i){hash^=uint8_t(identity[i]);hash*=16777619u;}
  snprintf(out,n,"%s%08lx%u",kind,(unsigned long)hash,slot);
}
static void localKey(const char *kind,uint8_t slot,char *out,size_t n){serialKey(serial,kind,slot,out,n);}
static void loadLocal(){
  memset(&aliases,0,sizeof(aliases));aliases.version=1;memcpy(aliases.serial,serial,17);
  char key[16];localKey("a",0,key,sizeof(key));Aliases stored{};
  if(preferences.getBytesLength(key)==sizeof(stored)&&preferences.getBytes(key,&stored,sizeof(stored))==sizeof(stored)&&stored.version==1&&memcmp(stored.serial,serial,17)==0){
    for(unsigned s=0;s<8;++s){stored.full[s][31]=stored.shortName[s][31]=0;
      if(stored.full[s][0]&&!PanelControls::validStoredName(stored.full[s],strlen(stored.full[s])))stored.full[s][0]=0;
      if(stored.shortName[s][0]&&!PanelControls::validStoredName(stored.shortName[s],strlen(stored.shortName[s])))stored.shortName[s][0]=0;}
    aliases=stored;
  }
  inputMenu=PanelInputMenu();inputMenuDirty=false;InputMenuRecord menuRecord;localKey("i",0,key,sizeof(key));
  if(preferences.getBytesLength(key)==sizeof(menuRecord)&&preferences.getBytes(key,&menuRecord,sizeof(menuRecord))==sizeof(menuRecord)&&menuRecord.version==1&&memcmp(menuRecord.serial,serial,17)==0&&menuRecord.menu.valid())inputMenu=menuRecord.menu;
  roleSlot=255;panelPresetChanged(dspi.activePreset);
}
static void capabilityService(){
  if(capabilityStep==0){evidence=PanelControls::CapabilityEvidence();platformByte=255;reportedOutputs=0;model.map=PanelControls::ChannelMap();control.map=model.map;Reply r=query(0x7f,0,7);if(r.status==WireStatus::Ok&&r.size>=4){platformByte=r.bytes[0];reportedOutputs=r.bytes[3];}++capabilityStep;return;}
  if(capabilityStep==1){Reply r=query(0x0a,0,16);if(validReply(r,16)&&r.bytes[0]==3){evidence.exposedInputs=r.bytes[1];evidence.exposedOutputs=r.bytes[2];evidence.countsConfirmed=evidence.exposedOutputs==reportedOutputs;}++capabilityStep;return;}
  if(capabilityStep==2){Reply r=query(0x86,0xffff,52);
    evidence.platform=platformByte==0?PanelControls::Platform::Rp2040:platformByte==1?PanelControls::Platform::Rp2350:PanelControls::Platform::Unknown;
    evidence.fiveBitEqConfirmed=validReply(r,52)&&r.bytes[0]==20&&r.bytes[2]==11&&r.bytes[3]>=79;
    model.map=PanelControls::channelMap(PanelControls::selectProfile(evidence));control.map=model.map;
    capabilityAttempted=true;++capabilityStep;clearCells();return;}
}
static String outputLabel(uint8_t out){uint8_t id=model.map.inputs+out;return id<17&&namesKnown[id]&&channelNames[id][0]?String(channelNames[id]):String("Output ")+String(out+1);}
static String inputLabel(uint8_t in){return in<17&&namesKnown[in]&&channelNames[in][0]?String(channelNames[in]):String("Input ")+String(in+1);}
static uint8_t aliasSourceAt(uint8_t row){for(uint8_t source=0;source<=SRC_MAX;++source)if(inputSourceAvailable(static_cast<InputSource>(source))){if(!row--)return source;}return 255;}
static uint8_t aliasSourceCount(){uint8_t n=0;for(uint8_t source=0;source<=SRC_MAX;++source)n+=inputSourceAvailable(static_cast<InputSource>(source));return n?n:1;}
static uint8_t availableSourceMask(){uint8_t mask=0;for(uint8_t s=0;s<=SRC_MAX;++s)if(inputSourceAvailable(static_cast<InputSource>(s)))mask|=1u<<s;return mask;}
static bool readName(uint8_t id){if(id>=17||namesKnown[id])return false;Reply r=query(0x9c,id,32);namesKnown[id]=true;
  if(validReply(r,32)){size_t n=0;while(n<32&&r.bytes[n])++n;PanelControls::displayName(r.bytes,r.size,channelNames[id],32,31);namesEditable[id]=n<=31&&PanelControls::validStoredName(reinterpret_cast<const char*>(r.bytes),n)&&strlen(channelNames[id])==n&&memcmp(channelNames[id],r.bytes,n)==0;}return true;}
static const char *filterName(uint8_t type){static const char *v[]={"Off","Peaking","Low Shelf","High Shelf","Low Pass","High Pass","Notch","All Pass","All Pass 1","Low Shelf 1","High Shelf 1","Linkwitz","Low Pass 1","High Pass 1"};return type<14?v[type]:"Crossover";}
static String xoValue(uint8_t row,float value){if(row==0)return value?"On":"Off";if(row==1)return value?"High Pass":"Low Pass";if(row==2)return uint8_t(value)==0?"Linkwitz":uint8_t(value)==1?"Butterworth":"Bessel";return String(row==3?value*6:value,0);}
static uint8_t bandRows(){
  if(!cells[0].known)return 1;uint8_t type=uint8_t(cells[0].value);
  if(type==11)return 6;const uint8_t bits=PanelControls::filterFields(type);
  return 2+!!(bits&PanelControls::FieldFrequency)+!!(bits&PanelControls::FieldGain)+!!(bits&PanelControls::FieldQ);
}
static PanelControls::BandField bandField(uint8_t row){
  if(row==0)return PanelControls::BandField::Bypass;if(row==1)return PanelControls::BandField::Type;
  if(uint8_t(cells[0].value)==11){static const PanelControls::BandField fields[]={PanelControls::BandField::Frequency,PanelControls::BandField::Q,PanelControls::BandField::TargetFrequency,PanelControls::BandField::TargetQ};return fields[row-2];}
  uint8_t at=2,bits=PanelControls::filterFields(uint8_t(cells[0].value));
  if((bits&PanelControls::FieldFrequency)&&row==at++)return PanelControls::BandField::Frequency;
  if((bits&PanelControls::FieldGain)&&row==at++)return PanelControls::BandField::GainDb;
  return PanelControls::BandField::Q;
}
static float bandValue(PanelControls::BandField f){switch(f){case PanelControls::BandField::Type:return cells[0].value;case PanelControls::BandField::Frequency:return cells[1].value;case PanelControls::BandField::Q:return cells[2].value;
  case PanelControls::BandField::GainDb:case PanelControls::BandField::TargetFrequency:return cells[3].value;case PanelControls::BandField::Bypass:return cells[4].value;case PanelControls::BandField::TargetQ:return PanelControls::decodeTargetQ(cells[5].value);default:return 0;}}
static bool bandService(){if(readField>=6)return false;uint16_t s=0;
  if(!PanelControls::eqSelector(model.map,eqId(),eqBand(),static_cast<PanelControls::ScalarField>(readField),s))return false;
  Reply r=query(0x43,s,4);Cell &c=cells[readField];c.attempted=true;
  if(validReply(r,4)){c.value=readField==0||readField==4?r.bytes[0]:readField==5?float(r.bytes[0]|(uint16_t(r.bytes[1])<<8)):PanelControlMenus::wireFloat(r.bytes);c.known=PanelControls::finite(c.value);}++readField;return true;}
static void startConfirmation(ConfirmAction action){returnPage=menuPage;returnRow=menuIndex;confirmation=action;model.confirmParent=page(menuPage);enterPage(PAGE_PANEL_CONFIRM);menuIndex=2;drawMenu();}
static bool guardedStop(){
  if(mediaPlayerPoc.active()||mediaPlaybackSuspended||mediaCurrentPath[0]||mediaTrackTransitionActive())stopMediaPlayback("panel hardware/preset action");
  return !mediaPlayerPoc.active()&&!mediaTrackTransitionActive()&&!mediaPlayerPoc.seeking()&&!mediaPlaybackBufferLow()&&!mediaRouteRestorePending;
}
static uint16_t rolesMask(uint8_t row){return row==1||row==2?model.roles.subs:(model.roles.left|model.roles.right);}
static void beginLocal(float value){localValue=originalValue=value;editGeneration=generation;editPage=menuPage;editRow=menuIndex;editActive=true;awaitingEdit=false;drawMenu();}
static bool openDecimal(float value,float low,float high,uint8_t dp,const char *unit){
  if(!number.begin(value,low,high,dp)){showToast("Outside panel range - Console");return true;}
  numberUnit=unit;numberParent=menuPage;numberRow=menuIndex;editGeneration=generation;
  enterPage(PAGE_PANEL_NUMBER);editPage=menuPage;editRow=0;editActive=true;uiView=VIEW_EDIT;drawMenu();return true;
}
static bool openNumber(float value){
  auto f=menuPage==PAGE_EQ_BAND?bandField(menuIndex):PanelControls::BandField::Frequency;
  if(f==PanelControls::BandField::Type||f==PanelControls::BandField::Bypass)return false;
  float low=10,high=control.sampleRate*(uint8_t(cells[0].value)==11?.15f:.45f);uint8_t dp=1;numberUnit="Hz";
  if(f==PanelControls::BandField::GainDb){low=-24;high=12;dp=2;numberUnit="dB";}
  if(f==PanelControls::BandField::Q||f==PanelControls::BandField::TargetQ){low=.1f;high=20;dp=2;numberUnit="";}
  // Native Qp0.10 rounds to51/512. Normalize only that valid boundary;
  // no-op Apply of0.10 re-encodes identically to51 (not a subminimum float).
  const bool quantizedMinimum=f==PanelControls::BandField::TargetQ&&value==51.0f/512.0f;
  return openDecimal(quantizedMinimum?.1f:value,low,high,dp,numberUnit);
}
static void closeNumber(){menuPage=numberParent;menuIndex=numberRow;editPage=numberParent;editRow=numberRow;editActive=false;uiView=VIEW_MENU;}
static void resultToast(){
  if(operationCancelled){showToast("Stopped - verify changed fields");return;}
  const Completion c=control.completion;
  if(c==Completion::Confirmed)showToast(hardwareAction?"Requested":"Applied");
  else if(c==Completion::Unknown)showToast("Unknown - verify state");
  else if(control.nativeStatus==2)showToast("In Use");
  else if(control.nativeStatus==4)showToast("In Use - active");
  else showToast(confirmedTargets?"Partial - verify state":"Rejected / changed");
}
} // namespace PanelRuntime

bool panelOwnsPage(MenuPage p){return p>=PAGE_SPEAKERS&&p<PAGE_COUNT;}
String panelPageTitle(MenuPage p){using namespace PanelRuntime;static const char *titles[]={"Speakers","Speaker Setup","Groups","Assign Outputs","Outputs","Output","Routing","Route","Sum Mode","Source Pair","Equaliser","Inputs","Outputs","Equaliser","Bands","Band","Crossover","Crossover","Preset Tools","Choose Slot","Preset Preview","Starting Point","Input Names","Input Name","Hardware","Stereo Outputs","Stereo Slot","PDM Out","S/PDIF Inputs","S/PDIF Input","I2S Input","I2S Data Pins","I2S Clocks","Master Clock","ADAT","Confirm","Edit Name"};
  if(p==PAGE_SPEAKERS)return "Quick Controls";
  if(p==PAGE_SPEAKER_SETUP)return "Output Setup";
  if(p==PAGE_INPUT_NAMES)return "Inputs";
  if(p==PAGE_INPUT_NAME)return panelSourceLabel(model.source,inputSourceDisplayText(static_cast<InputSource>(model.source)),false);
  if(p==PAGE_EQ_INPUTS)return "Input EQ";
  if(p==PAGE_COPY_DESTINATION)return "Copy To";
  if(p==PAGE_COPY_PREVIEW)return "Copy Parameters";
  if(p==PAGE_PANEL_NUMBER)return panelMenuName(numberParent,numberRow);
  if(p==PAGE_EQ_TARGET)return model.eqOutput?outputLabel(model.target):inputLabel(model.target);
  if(p==PAGE_PANEL_CONFIRM&&confirmation==ConfirmAction::Hardware)return String("DSP ")+String(field(editPage,editRow).label);
  if(p==PAGE_PANEL_CONFIRM&&confirmation==ConfirmAction::Sum)return "Replace Mono L+R";
  if(p==PAGE_PANEL_CONFIRM&&confirmation==ConfirmAction::LinkBand)return String("Copy ")+(linkedBand>=20?String("XO ")+String(linkedBand-19):String("Band ")+String(linkedBand+1));
  if(p==PAGE_PANEL_CONFIRM&&confirmation==ConfirmAction::CopyParameters)return "Copy Parameters";
  if(p==PAGE_OUTPUT)return outputLabel(model.output);if(p==PAGE_EQ_BAND)return String("Band ")+String(model.band+1);if(p==PAGE_XO_BAND)return String("XO ")+String(model.band+1);return titles[int(p)-int(PAGE_SPEAKERS)];}
uint8_t panelMenuCount(MenuPage p){using namespace PanelRuntime;if(p==PAGE_EQ_BAND)return bandRows();if(p==PAGE_INPUT_NAMES)return aliasSourceCount();return model.count(page(p));}
String panelMenuName(MenuPage p,uint8_t r){using namespace PanelRuntime;
  switch(p){case PAGE_SPEAKERS:{static const char *s[]={"Speakers","Subwoofer","Sub Level","Balance"};return s[r<4?r:3];}
    case PAGE_SPEAKER_SETUP:return r?"Restore Saved":"Speaker Groups";
    case PAGE_SPEAKER_GROUPS:return r==0?"Left":r==1?"Right":"Subs";
    case PAGE_OUTPUTS:if(r>=model.map.outputs){uint8_t rows=model.map.outputs?model.map.outputs:1;return r==rows?"Quick Controls":r==rows+1?"Setup":"Unavailable";}return outputLabel(r);
    case PAGE_SPEAKER_MEMBERS:case PAGE_EQ_OUTPUTS:return outputLabel(r);
    case PAGE_OUTPUT:{static const char *s[]={"Mute","Gain","Delay","EQ / Crossover","Routing","Rename","Enabled","Copy Parameters"};return s[r<8?r:7];}
    case PAGE_ROUTING:return r<model.visibleInputs()?inputLabel(r):String("Sum Mode");
    case PAGE_ROUTE:return r==0?"Enabled":r==1?"Gain":"Invert";
    case PAGE_SUM:return r==0?"Left":r==1?"Right":"Mono L+R";
    case PAGE_SOURCE_PAIR:return String("Inputs ")+String(r*2+1)+"/"+String(r*2+2);
    case PAGE_EQUALISER:return r?"Outputs":"Inputs";
    case PAGE_EQ_INPUTS:return inputLabel(r);
    case PAGE_EQ_TARGET:return r==0?"Bands":"Crossover";
    case PAGE_EQ_BANDS:return String("Band ")+String(r+1);
    case PAGE_EQ_BAND:{auto f=bandField(r);switch(f){case PanelControls::BandField::Type:return "Type";case PanelControls::BandField::Bypass:return "Enabled";
      case PanelControls::BandField::Frequency:return uint8_t(cells[0].value)==11?"f0":"Frequency";case PanelControls::BandField::Q:return uint8_t(cells[0].value)==11?"Q0":"Q";
      case PanelControls::BandField::TargetFrequency:return "fp";case PanelControls::BandField::TargetQ:return "Qp";default:return "Gain";}}
    case PAGE_CROSSOVER:return String("XO ")+String(r+1);
    case PAGE_XO_BAND:{static const char *s[]={"Enabled","Direction","Family","Slope","Frequency"};return s[r<5?r:4];}
    case PAGE_PRESET_TOOLS:{static const char *s[]={"Save Changes","Save As New","New Preset","Rename","Revert"};return s[r<5?r:4];}
    case PAGE_PRESET_DESTINATION:return presetListLabel(r);
    case PAGE_PRESET_PREVIEW:return r==0?"Destination":r==1?"Name":r==2?"Starting Point":r==3?"Output EQ / XO":r==4?"Wiring / Limiter":r==5?"Input PEQ / FX":"Continue";
    case PAGE_STARTING_POINT:return r?"Effects Off":"Current";
    case PAGE_INPUT_NAMES:{uint8_t source=aliasSourceAt(r);return source==255?String("Unavailable"):panelSourceLabel(source,inputSourceDisplayText(static_cast<InputSource>(source)),false);}
    case PAGE_INPUT_NAME:return r==0?"Name":r==1?"Short Name":r==2?"Show in Menu":r==3?"Position":"Reset Name";
    case PAGE_HARDWARE:{static const char *s[]={"Outputs","PDM Out","S/PDIF Inputs","I2S Input","I2S Clocks","Master Clock","ADAT","Preset Mode"};return s[r<8?r:7];}
    case PAGE_HARDWARE_OUTPUTS:return String("Slot ")+String(r*2+1)+"/"+String(r*2+2);
    case PAGE_SPDIF_INPUTS:return String("S/PDIF ")+String(r+1);
    case PAGE_I2S_INPUT:if(r==1)return "Data Pins";break;
    case PAGE_I2S_DATA:return String("Pair ")+String(r+1);
    case PAGE_PANEL_CONFIRM:if(confirmation==ConfirmAction::Sum)return r==0?"Left":r==1?"Right":r==2?"Cancel":"Save";if(confirmation==ConfirmAction::LinkBand)return r==0?"From":r==1?"Replace":r==2?"Cancel":"Copy Band";return r==0?"Current":r==1?"Proposed":r==2?"No - Cancel":"Yes - Apply";
    case PAGE_PANEL_NAME:return r==0?"Name":r==1?"Position":r==2?"Save Name":"Cancel";
    case PAGE_PANEL_NUMBER:return r==0?"Value":r==1?"Digit":r==2?"Apply":"Cancel";
    case PAGE_COPY_DESTINATION:return outputLabel(r);
    case PAGE_COPY_PREVIEW:return r==0?"From":r==1?"Replace":r==2?"EQ / Crossover":r==3?"Gain / Delay":r==4?"Mute":"Continue";
    default:break;
  }Field f=field(p,r);return f.label;
}
namespace PanelRuntime {
static String fieldValueText(const Field &f,float value){
  if(f.pin)return value==255?String("Unset"):String(int(value));
  if(f.get==0xc1)return value?"I2S":"S/PDIF";if(f.get==0xff)return value?"Split":"Unified";if(f.get==0xc9)return value?"256x":"128x";
  if(f.get==0x89)return value?"Slave":"Master";if(f.get==0x99)return value?"With Preset":"Independent";
  if(f.max==1&&f.min==0&&f.kind==ValueKind::Byte)return value?"On":"Off";
  if(f.get==0x6e||f.get==0xce)return String("State ")+String(int(value));
  if(f.kind==ValueKind::Float&&strcmp(f.unit,"dB")==0)return gainText(value);
  return String(value,f.step<1?1:0);
}
}
String panelMenuValue(MenuPage p,uint8_t r){using namespace PanelRuntime;
  if(p==PAGE_PANEL_CONFIRM){if(r>=2)return r==2?"Default":"Confirm";
    if(confirmation==ConfirmAction::CopyParameters)return String("Out ")+String((r?copyJob.target:copyJob.source)+1);
    if(confirmation==ConfirmAction::LinkBand)return String(model.eqOutput?"Out ":"In ")+String(int((r?linkedTarget:linkedSource)-(model.eqOutput?model.map.inputs:0))+1);
    if(confirmation==ConfirmAction::Hardware)return fieldValueText(field(editPage,editRow),r?localValue:originalValue);
    if(confirmation==ConfirmAction::Sum)return gainText(sumGains[r]);
    if(confirmation==ConfirmAction::Restore)return r?"Saved fields only":"Live adjustments";
    return r?String(presetSlot+1)+" "+String(presetNameDraft):String("P")+String(dspi.activePreset+1);}
  if(p==PAGE_PANEL_NAME)return r==0?panelNameWindow():r==1?String(name.cursor+1):r==2?"Explicit":"";
  if(p==PAGE_PANEL_NUMBER){char text[16];number.format(text,sizeof(text));return r==0?String(text):r==1?String(float(number.step())/number.scale,number.place<number.decimals?number.decimals-number.place:0):r==2?"Confirm":"";}
  if(p==PAGE_COPY_DESTINATION)return r==copyJob.source?"Source":"Choose";
  if(p==PAGE_COPY_PREVIEW)return r<2?String("Out ")+String((r?copyJob.target:copyJob.source)+1):r==2||r==3?"Replace":r==4?"Copy":"Confirm";
  if(editActive&&p==editPage&&r==editRow){if(p==PAGE_XO_BAND)return xoValue(r,localValue);if(p==PAGE_EQ_BAND){auto f=bandField(r);if(f==PanelControls::BandField::Bypass)return localValue?"On":"Off";if(f==PanelControls::BandField::Type)return filterName(uint8_t(localValue));if(f==PanelControls::BandField::GainDb)return gainText(localValue);return String(localValue,(f==PanelControls::BandField::Q||f==PanelControls::BandField::TargetQ)?2:1);}if(p==PAGE_INPUT_NAME)return r==2?(localValue?"On":"Off"):String(int(localValue));if(p==PAGE_SPEAKERS)return gainText(localValue);return fieldValueText(field(p,r),localValue);}
  if(p==PAGE_SPEAKERS){if(r>=4)return "Open";
    if(!rolesMask(r))return "Unassigned";if(r==2){if(PanelControls::bitCount(model.roles.subs)==1){uint8_t out=__builtin_ctz(model.roles.subs);return (gainKnownMask&(1u<<out))?gainText(freshGains[out]):String("Pending");}return subBaseline.valid?gainText(subTrim):String("Set");}if(r==3)return balanceBaseline.valid?gainText(balance):String("Set");
    uint16_t mask=rolesMask(r);unsigned muted=0,known=0;for(unsigned i=0;i<model.map.outputs;++i)if(mask&(1u<<i)){if(cells[i].known){++known;muted+=cells[i].value!=0;}}
    if(known!=PanelControls::bitCount(mask))return "Pending";return muted==0?"On":muted==known?"Off":"Mixed";}
  if(p==PAGE_SPEAKER_GROUPS){uint16_t mask=r==0?model.roles.left:r==1?model.roles.right:model.roles.subs;return mask?String(PanelControls::bitCount(mask)):String("Unassigned");}
  if(p==PAGE_SPEAKER_MEMBERS){uint16_t mask=model.group==0?model.roles.left:model.group==1?model.roles.right:model.roles.subs;return mask&(1u<<r)?"Assigned":"No";}
  if(p==PAGE_OUTPUTS&&r>=model.map.outputs)return r>=(model.map.outputs?model.map.outputs:1)?"Open":"Read-only";
  if(p==PAGE_OUTPUTS||p==PAGE_EQ_OUTPUTS)return cells[r].known?(cells[r].value?"On":"Off"):cells[r].attempted?"Unknown":"Pending";
  if(p==PAGE_SUM)return r>=2?String("Save"):!sumContext()?String("Reopen"):sumKnown&(1u<<r)?gainText(sumGains[r]):String(sumAttempted&(1u<<r)?"Unknown":"Pending");
  if(p==PAGE_SPEAKER_SETUP)return r?(savedContext()?"Confirm":"Unavailable"):"Open";
  if(p==PAGE_EQ_TARGET)return "Open";
  if(p==PAGE_EQ_BAND){if(readField<6)return "Pending";for(auto &c:cells)if(c.attempted&&!c.known)return "Unknown";auto f=bandField(r);float v=bandValue(f);if(f==PanelControls::BandField::Type)return filterName(uint8_t(v));if(f==PanelControls::BandField::Bypass)return cells[0].value!=0&&v==0?"On":"Off";if(f==PanelControls::BandField::GainDb)return gainText(v);return String(v,(f==PanelControls::BandField::Q||f==PanelControls::BandField::TargetQ)?2:1);}
  if(p==PAGE_XO_BAND){if(readField<6)return "Pending";if(!cells[0].known)return "Unknown";PanelControls::XoFamily family;uint8_t order;bool high;
    bool valid=PanelControls::decodeCrossover(cells[0].value,family,order,high);if(r==0)return xoValue(r,cells[0].value!=0&&cells[4].value==0);if(r==4)return xoValue(r,cells[1].value);if(!valid)return "Unset";return xoValue(r,r==1?float(high):r==2?float(uint8_t(family)):float(order));}
  if(p==PAGE_PRESET_TOOLS)return r==0||r==4?String(dspi.activePreset+1):String("Open");
  if(p==PAGE_PRESET_DESTINATION)return directoryKnown?((occupiedMask&(1u<<r))?"Occupied":"Empty"):"Unknown";
  if(p==PAGE_PRESET_PREVIEW)return r==0?String(presetSlot+1):r==1?String(presetNameDraft):r==2?(effectsOff?"Effects Off":"Current"):r==3||r==4?"Preserved":r==5?(effectsOff?"Bypass / Off":"Preserved"):"Confirm";
  if(p==PAGE_STARTING_POINT)return r==uint8_t(effectsOff)?"Selected":"";
  if(p==PAGE_INPUT_NAME){if(!serialKnown)return "Unknown identity";return r==0?String(aliases.full[model.source]):r==1?String(aliases.shortName[model.source]):r==2?((inputMenu.visible&(1u<<model.source))?"On":"Off"):r==3?(inputMenu.position(model.source,availableSourceMask())?String(inputMenu.position(model.source,availableSourceMask())):String("Hidden")):String("Confirm");}
  Field f=field(p,r);if(f.get){if(!model.map.writable)return "Read-only";if(!cells[r].attempted)return "Pending";if(!cells[r].known)return "Unknown";float v=cells[r].value;
    return fieldValueText(f,v);}
  if(!model.map.writable&&(p==PAGE_OUTPUTS||p==PAGE_EQ_INPUTS||p==PAGE_EQ_OUTPUTS||p==PAGE_HARDWARE_OUTPUTS))return "Read-only";
  return "Open";
}
const char *panelMenuUnit(MenuPage p,uint8_t r){using namespace PanelRuntime;
  if(p==PAGE_PANEL_NUMBER)return r==0?numberUnit:"";
  if((p==PAGE_SUM&&r<2&&sumKnown&(1u<<r))||(p==PAGE_PANEL_CONFIRM&&confirmation==ConfirmAction::Sum&&r<2))return "dB";
  String shown=panelMenuValue(p,r);const char *t=shown.c_str();bool digit=false;
  for(unsigned i=0;t[i];++i){if(t[i]>='0'&&t[i]<='9')digit=true;else if(t[i]!='.'&&t[i]!='-'&&t[i]!='+')return "";}if(!digit)return "";
  if(p==PAGE_SPEAKERS&&(r==2||r==3))return "dB";if(p==PAGE_EQ_BAND){auto f=bandField(r);return f==PanelControls::BandField::GainDb?"dB":f==PanelControls::BandField::Frequency||f==PanelControls::BandField::TargetFrequency?"Hz":"";}
  if(p==PAGE_XO_BAND)return r==4?"Hz":r==3?"dB/oct":"";return field(p,r).unit;}
int8_t panelMainIndex(MenuPage p){using namespace PanelRuntime;if(p==PAGE_PANEL_NUMBER)return panelMainIndex(numberParent);Page q=page(p);for(unsigned i=0;i<8;++i){if(q==Page::Effects||q==Page::Equaliser)return 4;if(q==Page::Preset||q==Page::PresetTools)return 2;if(q==Page::System||q==Page::Hardware||q==Page::InputNames)return 5;if(q==Page::Speakers||q==Page::Outputs)return 3;q=model.parent(q);}return 3;}
void panelEnterPage(MenuPage p){using namespace PanelRuntime;
  if(p==PAGE_PANEL_NUMBER)return; // retain originating confirmed recipe until Apply
  awaitingEdit=false;if(control.busy()&&!control.sent())control.cancel();cachePage=uint16_t(p);clearCells();statusText="";
  if(!panelOwnsPage(p))return;
  if(p==PAGE_EQ_TARGET)model.link=false;
  if(p==PAGE_SUM){if(sourcePair*2+1>=model.visibleInputs())sourcePair=0;sumOutput=model.output;sumPair=sourcePair;sumGeneration=generation;sumKnown=sumAttempted=0;}
  if(p==PAGE_SPEAKERS){gainKnownMask=gainAttemptedMask=0;}
  if(p==PAGE_PRESET_DESTINATION)directoryKnown=false;
  if(p==PAGE_PANEL_CONFIRM||p==PAGE_PANEL_NAME)return;
}
namespace PanelRuntime {
static bool allBandKnown(){if(readField<6)return false;for(unsigned i=0;i<6;++i)if(!cells[i].known)return false;return true;}
static bool openName(uint8_t purpose,const char *text,Page parent){
  if(!name.begin(text,31)){showToast("Name needs Console ASCII");return false;}
  namePurpose=purpose;model.nameParent=parent;editGeneration=generation;enterPage(PAGE_PANEL_NAME);menuIndex=0;drawMenu();return true;
}
static bool beginFieldEdit(){
  Field f=field(menuPage,menuIndex);
  if(!model.map.writable){showToast("Read-only firmware");return false;}
  if(menuPage==PAGE_ADAT&&evidence.platform!=PanelControls::Platform::Rp2350){showToast("ADAT unavailable on this DSP");return false;}
  if(!f.set||f.readonly){showToast(f.get==0xc1?"Format clocks need Console preflight":f.hardware?"Console-owned / read-only":"Unavailable");return false;}
  if(!cells[menuIndex].known){showToast("Wait for confirmed read");return false;}
  if(!(f.pin&&cells[menuIndex].value==255)&&(cells[menuIndex].value<f.min||cells[menuIndex].value>f.max)){showToast("Outside panel range - Console");return false;}
  if(f.kind==ValueKind::Float)return openDecimal(cells[menuIndex].value,f.min,f.max,strcmp(f.unit,"dB")==0?2:1,f.unit);
  beginLocal(cells[menuIndex].value);return true;
}
static bool startFieldWrite(MenuPage p,uint8_t row,float desired){
  Field f=field(p,row);if(!f.set||!model.map.writable)return false;
  if(p==PAGE_ADAT&&evidence.platform!=PanelControls::Platform::Rp2350)return false;
  if(f.pin){if(!uartKnown||!PanelControls::gpioValid(evidence.platform,uint8_t(desired))||uint8_t(desired)==uartTx||uint8_t(desired)==uartRx){showToast("Protected / invalid DSP GPIO");return false;}}
  hardwareAction=f.hardware;confirmedTargets=0;operationCancelled=false;
  if(p==PAGE_ROUTE)return control.startRoute(model.input,model.output,row==0?PanelControls::RouteField::Enabled:row==1?PanelControls::RouteField::Gain:PanelControls::RouteField::Invert,desired);
  if(f.action){uint16_t w=uint16_t(desired),readSelector=f.selector;
    if(f.set==0x7c||f.set==0xc0)w=(uint16_t(desired)<<8)|f.selector;
    if(f.set==0xe4||f.set==0xf1||f.set==0xe9||f.set==0xc2)w=(f.selector<<8)|uint16_t(desired);
    if(f.set==0xe9){bool ok=control.startAction(f.set,f.get,w,desired,f.kind,0);if(ok){control.readBytes=6;control.readOffset=1;control.readMask=1u<<model.source;}return ok;}
    return control.startAction(f.set,f.get,w,desired,f.kind,readSelector);
  }
  bool ok=control.startScalar(f.get,f.set,f.selector,desired,f.kind);
  if(ok&&f.get==0xee){control.readBytes=8;control.readOffset=4;}return ok;
}
static void startRoleRead(uint8_t job,uint16_t mask){groupJob=job;groupMask=mask;groupAt=0;confirmedTargets=0;operationCancelled=false;}
static bool launchLinkedSource(){linkStage=0;return linkedXoEnable?control.startXoEnabled(linkedSource,linkedBand,linkedValue==0):control.startBand(linkedSource,linkedBand,linkedField,linkedValue);}
static bool startBandWrite(float value){
  PanelControls::BandField f=menuPage==PAGE_EQ_BAND?bandField(menuIndex):PanelControls::BandField::Frequency;
  if((menuPage==PAGE_XO_BAND&&menuIndex==0)||(menuPage==PAGE_EQ_BAND&&f==PanelControls::BandField::Bypass)){
    bool current=cells[0].value!=0&&cells[4].value==0;
    if((value!=0)==current&&!model.link){showToast("Unchanged");return true;}
    if(menuPage==PAGE_XO_BAND){hardwareAction=false;confirmedTargets=0;operationCancelled=false;
      if(!model.link)return control.startXoEnabled(eqId(),eqBand(),value!=0);
    }
    if(menuPage==PAGE_EQ_BAND&&value&&cells[0].value==0){showToast("Choose filter Type first");return false;}value=value?0:1;
  }
  if(menuPage==PAGE_XO_BAND){
    uint8_t type=uint8_t(cells[0].value);PanelControls::XoFamily family=PanelControls::XoFamily::LinkwitzRiley;uint8_t order=2;bool high=false;
    PanelControls::decodeCrossover(type,family,order,high);
      if(menuIndex==0)f=PanelControls::BandField::Bypass;
    else if(menuIndex==4)f=PanelControls::BandField::Frequency;
    else {if(menuIndex==1)high=value!=0;if(menuIndex==2)family=static_cast<PanelControls::XoFamily>(uint8_t(value));if(menuIndex==3)order=uint8_t(value);
      if(family!=PanelControls::XoFamily::Butterworth&&order%2)order=2;
      uint8_t packed=0;if(!PanelControls::crossoverType(family,order*6,high,packed)){showToast("Unsupported native slope");return false;}f=PanelControls::BandField::Type;value=packed;}
  }
  hardwareAction=false;confirmedTargets=0;operationCancelled=false;
  if(model.link){
    const uint8_t partner=model.target^1,limit=model.eqOutput?model.map.outputs:model.visibleInputs();
    if(partner>=limit)return false;
    linkedSource=eqId();linkedTarget=(model.eqOutput?model.map.inputs:0)+partner;linkedBand=eqBand();linkedField=f;linkedValue=value;
    linkType=uint8_t(cells[0].value);linkGeneration=generation;linkedCopy=linkedSecond=false;linkedXoEnable=menuPage==PAGE_XO_BAND&&menuIndex==0;linkStage=1;return true;
  }
  return control.startBand(eqId(),eqBand(),f,value);
}
static bool linkService(){
  if(linkStage!=1)return false;
  if(linkGeneration!=generation){linkStage=0;linkedTarget=255;showToast("Changed - edit again");return true;}
  uint16_t selector=0;if(!PanelControls::eqSelector(model.map,linkedTarget,linkedBand,PanelControls::ScalarField::Type,selector)){linkStage=0;linkedTarget=255;return true;}
  Reply r=query(0x43,selector,4);if(!validReply(r,4)||linkGeneration!=generation){linkStage=0;linkedTarget=255;showToast("Linked channel unavailable");return true;}
  if(r.bytes[0]!=linkType){linkStage=2;startConfirmation(ConfirmAction::LinkBand);showToast("Different band - copy required");}
  else if(!launchLinkedSource()){linkedTarget=255;showToast("Linked edit rejected");}return true;
}
static void beginPresetFlow(uint8_t tool){presetTool=tool;effectsOff=false;presetSlot=dspi.activePreset;strncpy(presetNameDraft,dspi.presetNames[presetSlot],31);presetNameDraft[31]=0;
  if(tool==0||tool==4){startConfirmation(tool==0?ConfirmAction::Save:ConfirmAction::Revert);return;}
  enterPage(PAGE_PRESET_DESTINATION);menuIndex=0;drawMenu();
}
static bool directoryRead(){Reply r=query(0x95,0,7);directoryKnown=false;
  if(validReply(r,7)){occupiedMask=r.bytes[0]|(uint16_t(r.bytes[1])<<8);directoryKnown=(occupiedMask&~uint16_t(0x3ff))==0&&r.bytes[6]<=1;}
  return directoryKnown;
}
}
bool panelWorkPending(){using namespace PanelRuntime;return control.busy()||copyJob.stage||linkStage==1||hardwareGuard||groupJob||presetStage||nameStage||sumStage;}
bool panelSelect(){using namespace PanelRuntime;
  if(menuPage==PAGE_PRESET&&menuIndex==10){enterPage(PAGE_PRESET_TOOLS);return true;}
  if(!panelOwnsPage(menuPage))return false;
  if(panelWorkPending()){showToast("Pending - wait or Back");drawMenu();return true;}
  if(editActive){panelApply();drawMenu();return true;}
  const uint8_t r=menuIndex;
  switch(menuPage){
    case PAGE_SPEAKERS:
      if(!rolesMask(r)){showToast("Assign roles in Setup");break;}
      if(!model.map.writable){showToast("Read-only firmware");break;}
      if(r<2){bool allMuted=true,allKnown=true;for(unsigned i=0;i<model.map.outputs;++i)if(rolesMask(r)&(1u<<i)){allKnown&=cells[i].known;allMuted&=cells[i].value!=0;}
        if(!allKnown){showToast("Wait for confirmed reads");break;}localValue=allMuted?0:1;startRoleRead(1,rolesMask(r));break;}
      startRoleRead(r==2?2:3,rolesMask(r));break;
    case PAGE_SPEAKER_SETUP:if(!r){enterPage(PAGE_SPEAKER_GROUPS);return true;}if(savedContext())startConfirmation(ConfirmAction::Restore);else showToast("No confirmed saved adjustments");break;
    case PAGE_SPEAKER_GROUPS:model.group=r;enterPage(PAGE_SPEAKER_MEMBERS);return true;
    case PAGE_SPEAKER_MEMBERS:{if(!serialKnown||!model.map.writable){showToast("Confirmed identity required");break;}
      uint16_t mask=model.group==0?model.roles.left:model.group==1?model.roles.right:model.roles.subs;
      if(!model.assignRole(model.group,r,(mask&(1u<<r))==0)){showToast("Already assigned to another role");break;}
      if(dspi.activePreset<10){RoleDraft &draft=roleDrafts[dspi.activePreset];memcpy(draft.record.serial,serial,17);draft.record.roles=model.roles;draft.dirty=true;rolesDirty=true;}
      gainBaseline.valid=subBaseline.valid=balanceBaseline.valid=false;savedKnown=false;markDeferredPreference(PREF_DIRTY_PANEL_CONTROLS);break;}
    case PAGE_OUTPUTS:{uint8_t rows=model.map.outputs?model.map.outputs:1;if(r>=rows){enterPage(r==rows?PAGE_SPEAKERS:PAGE_SPEAKER_SETUP);return true;}if(!model.map.writable||r>=model.map.outputs){showToast("Read-only firmware");break;}model.output=r;enterPage(PAGE_OUTPUT);return true;}
    case PAGE_OUTPUT:
      if(r==7){if(!model.map.writable||!serialKnown){showToast("Confirmed identity required");break;}copyJob.source=model.output;copyJob.generation=generation;enterPage(PAGE_COPY_DESTINATION);return true;}
      if(r==5){uint8_t id=model.map.inputs+model.output;if(!model.map.writable||!namesKnown[id]){showToast("Wait for name read");break;}if(!namesEditable[id]){showToast("Native name needs Console ASCII");break;}openName(2,channelNames[id],Page::Output);return true;}
      if(r==4){enterPage(PAGE_ROUTING);return true;}
      if(r==3){model.eqOutput=true;model.target=model.output;model.link=false;model.eqParent=Page::Output;enterPage(PAGE_EQ_TARGET);return true;}
      beginFieldEdit();break;
    case PAGE_ROUTING:if(r>=model.visibleInputs()){sourcePair=0;enterPage(model.visibleInputs()>2?PAGE_SOURCE_PAIR:PAGE_SUM);}else{model.input=r;enterPage(PAGE_ROUTE);}return true;
    case PAGE_SUM:if(!sumReady()){showToast("Wait for current routes / reopen");break;}if(r<2){openDecimal(sumGains[r],-60,0,2,"dB");return true;}startConfirmation(ConfirmAction::Sum);return true;
    case PAGE_SOURCE_PAIR:sourcePair=r;enterPage(PAGE_SUM);return true;
    case PAGE_EQUALISER:enterPage(r?PAGE_EQ_OUTPUTS:PAGE_EQ_INPUTS);return true;
    case PAGE_EQ_INPUTS:case PAGE_EQ_OUTPUTS:if(!model.map.writable){showToast("Read-only firmware");break;}model.eqOutput=menuPage==PAGE_EQ_OUTPUTS;model.target=r;model.link=false;model.eqParent=page(menuPage);enterPage(PAGE_EQ_TARGET);return true;
    case PAGE_EQ_TARGET:enterPage(r==0?PAGE_EQ_BANDS:PAGE_CROSSOVER);return true;
    case PAGE_EQ_BANDS:model.band=r;enterPage(PAGE_EQ_BAND);return true;
    case PAGE_CROSSOVER:model.band=r;enterPage(PAGE_XO_BAND);return true;
    case PAGE_EQ_BAND:if(!allBandKnown()){showToast("Wait for confirmed band");break;}if(openNumber(bandValue(bandField(r))))return true;beginLocal(bandField(r)==PanelControls::BandField::Bypass?(cells[0].value!=0&&cells[4].value==0?1:0):bandValue(bandField(r)));break;
    case PAGE_XO_BAND:{if(!allBandKnown()){showToast("Wait for confirmed crossover");break;}PanelControls::XoFamily family=PanelControls::XoFamily::LinkwitzRiley;uint8_t order=2;bool high=false;PanelControls::decodeCrossover(cells[0].value,family,order,high);
      if(r==4){openNumber(cells[1].value);return true;}beginLocal(r==0?(cells[0].value!=0&&cells[4].value==0?1:0):r==1?float(high):r==2?float(uint8_t(family)):r==3?float(order):cells[1].value);break;}
    case PAGE_PANEL_NUMBER:
      if(r==3){closeNumber();drawMenu();return true;}
      if(r<2){editActive=true;editPage=menuPage;editRow=r;break;}
      localValue=number.value();closeNumber();panelApply();drawMenu();return true;
    case PAGE_COPY_DESTINATION:
      if(r==copyJob.source||r>=model.map.outputs){showToast("Choose another output");break;}
      if(copyJob.generation!=generation){showToast("Changed - reopen copy");break;}copyJob.target=r;enterPage(PAGE_COPY_PREVIEW);return true;
    case PAGE_COPY_PREVIEW:if(r==5){startConfirmation(ConfirmAction::CopyParameters);return true;}break;
    case PAGE_PRESET_TOOLS:if(!dspi.connected){showToast("No DSPi");break;}beginPresetFlow(r);return true;
    case PAGE_PRESET_DESTINATION:if(!directoryKnown){showToast("Unknown directory");break;}
      if(presetTool==3){if(!(occupiedMask&(1u<<r))){showToast("Choose occupied slot");break;}presetSlot=r;strncpy(presetNameDraft,dspi.presetNames[r],31);openName(3,presetNameDraft,Page::PresetPreview);return true;}
      if(occupiedMask&(1u<<r)){showToast("Choose empty slot");break;}presetSlot=r;strcpy(presetNameDraft,"New Preset");openName(3,presetNameDraft,Page::PresetPreview);return true;
    case PAGE_PRESET_PREVIEW:
      if(r==1){openName(3,presetNameDraft,Page::PresetPreview);return true;}
      if(r==2&&presetTool==2){enterPage(PAGE_STARTING_POINT);return true;}
      if(r==6){startConfirmation(presetTool==3?ConfirmAction::Rename:presetTool==2?ConfirmAction::New:ConfirmAction::SaveNew);return true;}break;
    case PAGE_STARTING_POINT:effectsOff=r!=0;enterPage(PAGE_PRESET_PREVIEW);return true;
    case PAGE_INPUT_NAMES:{uint8_t source=aliasSourceAt(r);if(source==255){showToast("Source unavailable");break;}model.source=source;enterPage(PAGE_INPUT_NAME);return true;}
    case PAGE_INPUT_NAME:if(!serialKnown){showToast("Unknown DSP identity");break;}if(r==2){beginLocal((inputMenu.visible&(1u<<model.source))?1:0);break;}if(r==3){uint8_t position=inputMenu.position(model.source,availableSourceMask());if(!position){showToast("Show input before ordering");break;}beginLocal(position);break;}if(r==4){aliases.full[model.source][0]=aliases.shortName[model.source][0]=0;aliasesDirty=true;markDeferredPreference(PREF_DIRTY_PANEL_CONTROLS);showToast("Default name - pending storage");break;}
      openName(r,r?aliases.shortName[model.source]:aliases.full[model.source],Page::InputName);return true;
    case PAGE_HARDWARE:{static const MenuPage pages[]={PAGE_HARDWARE_OUTPUTS,PAGE_PDM,PAGE_SPDIF_INPUTS,PAGE_I2S_INPUT,PAGE_I2S_CLOCKS,PAGE_MASTER_CLOCK,PAGE_ADAT};if(r<7){enterPage(pages[r]);return true;}beginFieldEdit();break;}
    case PAGE_HARDWARE_OUTPUTS:model.pair=r;enterPage(PAGE_HARDWARE_PAIR);return true;
    case PAGE_SPDIF_INPUTS:model.source=r;enterPage(PAGE_SPDIF_INPUT);return true;
    case PAGE_I2S_INPUT:if(r==1){enterPage(PAGE_I2S_DATA);return true;}beginFieldEdit();break;
    case PAGE_PANEL_NAME:if(r==3){name.cancel();enterPage(native(model.nameParent));return true;}if(r<2){editActive=true;editPage=menuPage;editRow=r;drawMenu();return true;}
      if(editGeneration!=generation){showToast("Changed - reopen name editor");break;}
      if(!name.save()){showToast("Enter a supported name");break;}
      if(namePurpose<2){uint16_t width=fontTextWidth(FontMedium,String(name.text));
        const int16_t allowed=std::max<int16_t>(0,302-fontTextWidth(FontMedium,presetText())-18-28-uiFeatureIconScaledSpan(UI_FEATURE_ALL,125,7));
        if(namePurpose==1&&width>allowed){name.active=true;showToast("Short name exceeds safe Home width");break;}
        strncpy(namePurpose?aliases.shortName[model.source]:aliases.full[model.source],name.text,32);aliasesDirty=true;markDeferredPreference(PREF_DIRTY_PANEL_CONTROLS);enterPage(PAGE_INPUT_NAME);showToast("Name applied - pending storage");return true;}
      if(namePurpose==3){strncpy(presetNameDraft,name.text,32);enterPage(PAGE_PRESET_PREVIEW);return true;}
      nameJobTarget=model.map.inputs+model.output;strncpy(nameJobValue,name.text,32);nameReturnAllowed=true;nameStage=1;showToast("Pending name read/write");break;
    case PAGE_PANEL_CONFIRM:
      if(r<2)break;
      if(r==2){if(confirmation==ConfirmAction::LinkBand){linkStage=0;linkedTarget=255;}if(confirmation==ConfirmAction::Sum){menuPage=returnPage;uiView=VIEW_MENU;editActive=false;}else enterPage(returnPage);menuIndex=returnRow;confirmation=ConfirmAction::None;drawMenu();return true;}
      if(confirmation==ConfirmAction::CopyParameters){
        if(copyJob.generation!=generation||!model.map.writable||!serialKnown||copyJob.target>=model.map.outputs||copyJob.source>=model.map.outputs||copyJob.source==copyJob.target){showToast("Changed - reopen copy");break;}
        copyJob.stage=1;copyJob.read=copyJob.write=0;operationCancelled=false;hardwareAction=false;confirmedTargets=0;enterPage(PAGE_OUTPUT);menuIndex=7;showToast("Reading source parameters");return true;
      }
      if(confirmation==ConfirmAction::LinkBand){if(linkStage!=2||linkGeneration!=generation){linkStage=0;linkedTarget=255;showToast("Changed - edit again");break;}linkedCopy=true;enterPage(returnPage);menuIndex=returnRow;if(!launchLinkedSource()){linkedTarget=255;showToast("Copy rejected");}return true;}
      if(confirmation==ConfirmAction::Hardware){if(!guardedStop()){showToast("Music stop failed");break;}if(editGeneration!=generation){showToast("Changed - edit again");break;}hardwareGuard=1;hardwareGeneration=generation;hardwareAction=true;hardwareLeaveMuted=hardwareCancel=false;hardwareReadbacks=0;enterPage(editPage);menuIndex=editRow;showToast("Guarding hardware change");break;}
      if(confirmation==ConfirmAction::Restore){if(!savedContext()){showToast("Saved fields unavailable");break;}memcpy(plannedGains,savedGains,sizeof(plannedGains));startRoleRead(6,savedMask);enterPage(PAGE_SPEAKERS);break;}
      if(confirmation==ConfirmAction::Sum){if(!sumReady()){showToast("Routes changed - reopen Sum");break;}for(float v:sumGains)if(!PanelControls::finite(v)||v<-60||v>0){showToast("Gain outside panel range");return true;}sumStage=1;groupAt=0;confirmedTargets=0;operationCancelled=false;enterPage(PAGE_ROUTING);showToast("Replacing selected output routes");break;}
      if(!guardedStop()){showToast("Music stop failed");break;}
      if(confirmation==ConfirmAction::Revert){MenuPage target=PAGE_PRESET_TOOLS;if(dspi.activePreset<10)roleDrafts[dspi.activePreset].dirty=false;
        if(loadPreset(dspi.activePreset,false)){panelPresetLoaded(dspi.activePreset);enterPage(target);showToast("Reverted");}else showToast("Load unresolved");break;}
      if(!serialKnown){showToast("Confirmed DSP identity required");break;}
      if(confirmation==ConfirmAction::New&&effectsOff&&!model.map.writable){showToast("Input EQ capability unverified");break;}
      if(confirmation!=ConfirmAction::Rename&&!ensurePresetMasterVolumeModeVerified()){showToast("Master-volume policy unverified");break;}
      capturePresetSnapshot();presetCancelled=false;operationCancelled=false;presetJobAction=confirmation;presetJobSlot=presetSlot;presetActiveBefore=dspi.activePreset;strncpy(presetJobName,presetNameDraft,32);presetJobGeneration=generation;saveAttempted=saveAckKnown=renameAttempted=false;
      presetStage=1;cleanEffect=cleanInput=cleanBand=0;showToast("Pending - verify destination");break;
    default:beginFieldEdit();break;
  }
  drawMenu();return true;
}
void panelAdjust(int direction){using namespace PanelRuntime;
  if(menuPage==PAGE_INPUT_NAME){float next=localValue;PanelControlMenus::safeAdjust(localValue,direction,1,menuIndex==2?0:1,menuIndex==2?1:inputMenu.count(availableSourceMask()),next);localValue=next;return;}
  if(menuPage==PAGE_PANEL_NUMBER){if(editRow==1)number.move(direction>0?1:-1);else number.adjust(direction);return;}
  if(menuPage==PAGE_PANEL_NAME){if(editRow==1)name.move(direction>0?1:-1);else name.adjust(direction);return;}
  if(menuPage==PAGE_SPEAKERS){float low=menuIndex==2&&PanelControls::bitCount(model.roles.subs)==1?-60:-24;float high=menuIndex==2&&PanelControls::bitCount(model.roles.subs)==1?0:24;if(!PanelControlMenus::safeAdjust(localValue,direction,0.5f,low,high,localValue))showToast("Outside panel range - Console");return;}
  if(menuPage==PAGE_EQ_BAND){auto f=bandField(menuIndex);float low=0,high=1,step=1;
    if(f==PanelControls::BandField::Type)high=13;
    else if(f==PanelControls::BandField::Frequency||f==PanelControls::BandField::TargetFrequency){low=10;high=control.sampleRate*(uint8_t(cells[0].value)==11?0.15f:0.45f);step=localValue<100?1:localValue<1000?10:100;}
    else if(f==PanelControls::BandField::Q||f==PanelControls::BandField::TargetQ){low=.1f;high=20;step=.05f;}
    else if(f==PanelControls::BandField::GainDb){low=-24;high=12;step=.5f;}
    if(!PanelControlMenus::safeAdjust(localValue,direction,step,low,high,localValue))showToast("Outside panel range - Console");return;}
  if(menuPage==PAGE_XO_BAND){float low=0,high=1,step=1;if(menuIndex==2)high=2;else if(menuIndex==3){PanelControls::XoFamily family;uint8_t order;bool highPass;bool known=PanelControls::decodeCrossover(cells[0].value,family,order,highPass);low=known&&family==PanelControls::XoFamily::Butterworth?1:2;high=8;step=low==1?1:2;}else if(menuIndex==4){low=10;high=control.sampleRate*.45f;step=localValue<1000?10:100;}
    PanelControlMenus::safeAdjust(localValue,direction,step,low,high,localValue);return;}
  Field f=field(menuPage,menuIndex);float next=localValue;
  if(f.pin&&localValue==255){next=direction>0?f.min:f.max;while(next>=f.min&&next<=f.max&&(!PanelControls::gpioValid(evidence.platform,uint8_t(next))||uint8_t(next)==uartTx||uint8_t(next)==uartRx))next+=direction>0?1:-1;if(next>=f.min&&next<=f.max)localValue=next;return;}
  if(f.set==0xed){static const float rates[]={44100,48000,96000};unsigned at=0;while(at<3&&rates[at]!=localValue)++at;if(at==3){showToast("Unknown selected rate");return;}localValue=rates[(int(at)+(direction>0?1:2))%3];return;}
  if(!PanelControlMenus::safeAdjust(localValue,direction,f.step,f.min,f.max,next)){showToast("Outside panel range - Console");return;}
  if(f.pin){for(unsigned i=0;i<32;++i){if(PanelControls::gpioValid(evidence.platform,uint8_t(next))&&uint8_t(next)!=uartTx&&uint8_t(next)!=uartRx)break;float n=next;if(!PanelControlMenus::safeAdjust(next,direction,1,f.min,f.max,n)||n==next)return;next=n;}}
  localValue=next;
}
void panelApply(){using namespace PanelRuntime;
  if(menuPage==PAGE_PANEL_NUMBER){editActive=false;return;}
  if(menuPage==PAGE_PANEL_NAME){editActive=false;return;}
  if(editGeneration!=generation){editActive=false;showToast("Changed - read and edit again");return;}
  editActive=false;
  if(menuPage==PAGE_SUM){if(!sumReady()||menuIndex>=2){showToast("Routes changed - reopen Sum");return;}sumGains[menuIndex]=localValue;showToast("Draft - choose Mono L+R Save");return;}
  if(menuPage==PAGE_INPUT_NAME){if(!serialKnown){showToast("Unknown identity");return;}bool ok=menuIndex==2?inputMenu.setVisible(model.source,localValue!=0,availableSourceMask()):inputMenu.moveTo(model.source,uint8_t(localValue),availableSourceMask());if(!ok){showToast("Keep one available input visible");return;}inputMenuDirty=true;markDeferredPreference(PREF_DIRTY_PANEL_CONTROLS);showToast("Applied - pending storage");return;}
  if(menuPage==PAGE_SPEAKERS){startRoleRead(menuIndex==2?4:5,rolesMask(menuIndex));return;}
  if(menuPage==PAGE_EQ_BAND||menuPage==PAGE_XO_BAND){if(!startBandWrite(localValue))showToast("Invalid native field");return;}
  Field f=field(menuPage,menuIndex);if(f.pin&&localValue==255){showToast("Choose a valid DSP pin");return;}if(f.hardware){startConfirmation(ConfirmAction::Hardware);return;}
  if(!startFieldWrite(menuPage,menuIndex,localValue))showToast("Unavailable");
}
bool panelNameMove(int direction){using namespace PanelRuntime;if(menuPage==PAGE_PANEL_NUMBER){number.move(-direction);drawMenu();return true;}if(menuPage!=PAGE_PANEL_NAME)return false;name.move(direction);drawMenu();return true;}
bool panelEditUnderline(MenuPage p,uint8_t row){return editActive&&p==PanelRuntime::editPage&&row==PanelRuntime::editRow&&p!=PAGE_PANEL_NAME;}
int16_t panelNumberCursorOffset(){char text[16];PanelRuntime::number.format(text,sizeof(text));text[PanelRuntime::number.cursor(text)]=0;return fontTextWidth(FontMedium,String(text));}
int16_t panelNumberCursorWidth(){char text[16],digit[2]{};PanelRuntime::number.format(text,sizeof(text));digit[0]=text[PanelRuntime::number.cursor(text)];return fontTextWidth(FontMedium,String(digit));}
namespace PanelRuntime {
static uint16_t nameMeasure(const char *s,void *){return fontTextWidth(FontMedium,String(s));}
static void namePreview(char *out,uint16_t &offset,uint16_t &width){if(!name.preview(out,32,132,nameMeasure,nullptr,offset,width)){out[0]=0;offset=0;width=1;}}
}
String panelNameWindow(){char text[32];uint16_t offset,width;PanelRuntime::namePreview(text,offset,width);return String(text);}
int16_t panelNameCursorOffset(){char text[32];uint16_t offset,width;PanelRuntime::namePreview(text,offset,width);return offset;}
int16_t panelNameCursorWidth(){char text[32];uint16_t offset,width;PanelRuntime::namePreview(text,offset,width);return width;}
String panelSourceLabel(uint8_t source,const String &fallback,bool shortName){using namespace PanelRuntime;if(!serialKnown||source>=8)return fallback;const char *text=shortName?aliases.shortName[source]:aliases.full[source];return text[0]?String(text):fallback;}
uint8_t panelNextInput(uint8_t source,int direction){using namespace PanelRuntime;PanelInputMenu defaults;return (serialKnown?inputMenu:defaults).next(source,direction,availableSourceMask());}
void panelPresetChanged(uint8_t slot){using namespace PanelRuntime;
  if(slot>=10||roleSlot==slot)return;model.roles=PanelControlMenus::Roles();roleSlot=slot;gainBaseline.valid=subBaseline.valid=balanceBaseline.valid=false;savedKnown=false;savedEligible=false;balance=subTrim=0;
  if(!serialKnown)return;char key[16];localKey("r",slot,key,sizeof(key));RoleRecord record{};
  if(preferences.getBytesLength(key)==sizeof(record)&&preferences.getBytes(key,&record,sizeof(record))==sizeof(record)&&record.version==1&&memcmp(record.serial,serial,17)==0&&
      ((record.roles.left|record.roles.right|record.roles.subs)&~uint16_t(0x1ff))==0&&!(record.roles.left&record.roles.right)&&!(record.roles.left&record.roles.subs)&&!(record.roles.right&record.roles.subs))model.roles=record.roles;
  if(roleDrafts[slot].dirty&&memcmp(roleDrafts[slot].record.serial,serial,17)==0)model.roles=roleDrafts[slot].record.roles;
}
void panelPresetLoaded(uint8_t slot){using namespace PanelRuntime;roleSlot=255;panelPresetChanged(slot);savedEligible=false;}
bool panelSaveRoles(uint8_t slot){using namespace PanelRuntime;if(!serialKnown||slot>=10)return false;RoleRecord record;memcpy(record.serial,serial,17);record.roles=model.roles;char key[16];localKey("r",slot,key,sizeof(key));return preferences.putBytes(key,&record,sizeof(record))==sizeof(record);}
void panelLegacySaveCapture(uint8_t slot){using namespace PanelRuntime;
  savedKnown=savedEligible=false;legacySaveSlot=slot;legacySaveRolesKnown=serialKnown&&slot<10;
  if(legacySaveRolesKnown){memcpy(legacySaveRoles.serial,serial,17);legacySaveRoles.roles=model.roles;}
}
bool panelLegacySaveComplete(uint8_t slot,bool confirmed){using namespace PanelRuntime;
  if(!legacySaveRolesKnown)return true;legacySaveRolesKnown=false;
  if(!confirmed||slot!=legacySaveSlot||slot>=10||!serialKnown||memcmp(serial,legacySaveRoles.serial,17)!=0)return false;
  // Queue the immutable originating record through the existing stopped-only
  // preference service; completion may occur after Music has been restarted.
  roleDrafts[slot].record=legacySaveRoles;roleDrafts[slot].dirty=true;rolesDirty=true;
  if(serialKnown&&memcmp(serial,legacySaveRoles.serial,17)==0&&dspi.activePreset==slot){model.roles=legacySaveRoles.roles;roleSlot=slot;}
  markDeferredPreference(PREF_DIRTY_PANEL_CONTROLS);return true;
}
bool panelPersistLocal(){using namespace PanelRuntime;if(!serialKnown)return false;bool ok=true;
  if(inputMenuDirty){InputMenuRecord record;memcpy(record.serial,serial,17);record.menu=inputMenu;char key[16];localKey("i",0,key,sizeof(key));bool stored=preferences.putBytes(key,&record,sizeof(record))==sizeof(record);ok&=stored;if(stored)inputMenuDirty=false;}
  if(aliasesDirty){char key[16];localKey("a",0,key,sizeof(key));bool stored=preferences.putBytes(key,&aliases,sizeof(aliases))==sizeof(aliases);ok&=stored;if(stored)aliasesDirty=false;}
  if(rolesDirty){bool remaining=false;for(uint8_t slot=0;slot<10;++slot)if(roleDrafts[slot].dirty){char key[16];RoleDraft &draft=roleDrafts[slot];serialKey(draft.record.serial,"r",slot,key,sizeof(key));bool stored=preferences.putBytes(key,&draft.record,sizeof(draft.record))==sizeof(draft.record);if(stored)draft.dirty=false;else remaining=true;ok&=stored;}rolesDirty=remaining;}return ok;
}
void panelInvalidate(){using namespace PanelRuntime;if(wireActive){pendingInvalidation=true;return;}
  clearEqEchoes();
  sumKnown=0;
  ++generation;control.invalidate();gainBaseline.valid=subBaseline.valid=balanceBaseline.valid=false;gainKnownMask=0;savedKnown=false;savedEligible=false;directoryKnown=false;activeInputStep=0;memset(namesKnown,0,sizeof(namesKnown));clearCells();
  if(groupJob||sumStage||copyJob.stage||hardwareGuard||presetStage||nameStage||control.busy())operationCancelled=true;
  copyJob.stage=0;
  groupJob=sumStage=linkStage=0;linkedTarget=255;linkedSecond=false;hardwareGuard=0;hardwareCancel=true;
  if(presetStage)presetStage=saveAttempted?12:renameAttempted?16:0;
  if(nameStage<3)nameStage=0;externalChangeNotice=true;
}
void panelSourceChanged(){using namespace PanelRuntime;model.activeInputs=0;panelInvalidate();}
void panelNotification(const uint8_t *payload,uint16_t bytes){using namespace PanelRuntime;
  if(consumeEqEcho(payload,bytes))return;
  // Pinned v2 PARAM_CHANGED identifies exclusive UART-origin echoes with8.
  // Arrival during a transaction alone never identifies an own change.
  if(payload&&bytes>=12&&payload[0]==2&&payload[1]==2&&payload[8]==8){uint16_t size=payload[6]|(uint16_t(payload[7])<<8);if(size<=52&&bytes==12+size)return;}
  if(payload&&bytes>=2&&payload[0]==2&&(payload[1]==3||payload[1]==4))pendingRoleReload=true;
  if(wireActive)pendingInvalidation=true;else{panelInvalidate();if(pendingRoleReload){roleSlot=255;pendingRoleReload=false;}}
}
namespace PanelRuntime {static void drainInvalidation(){pendingInvalidation=false;panelInvalidate();if(pendingRoleReload){roleSlot=255;pendingRoleReload=false;}}}
void panelCancel(){using namespace PanelRuntime;awaitingEdit=false;name.cancel();
  if(copyJob.stage)operationCancelled=true;copyJob.stage=0;
  if(control.busy()||groupJob||sumStage)operationCancelled=true;
  control.cancel();linkedTarget=255;linkStage=0;linkedSecond=false;hardwareCancel=true;if(hardwareGuard<=2)hardwareGuard=0;groupJob=0;sumStage=0;
  if(!saveAttempted&&!renameAttempted)presetStage=0;else presetCancelled=true;
  if(nameStage<=2)nameStage=0;else nameReturnAllowed=false;editActive=false;}
bool panelBack(){using namespace PanelRuntime;if(!panelOwnsPage(menuPage))return false;
  if(menuPage==PAGE_PANEL_NUMBER){panelCancel();closeNumber();drawMenu();return true;}
  if(editActive){panelCancel();drawMenu();return true;}
  if(menuPage==PAGE_PANEL_NAME){panelCancel();enterPage(native(model.nameParent));return true;}
  if(menuPage==PAGE_PANEL_CONFIRM){const bool sum=confirmation==ConfirmAction::Sum;panelCancel();if(!presetStage)confirmation=ConfirmAction::None;if(sum){menuPage=returnPage;uiView=VIEW_MENU;}else enterPage(returnPage);menuIndex=returnRow;drawMenu();return true;}
  const bool resolving=control.busy()&&control.sent();panelCancel();if(resolving)showToast("Resolving sent change");
  enterPage(native(model.parent(page(menuPage))));return true;
}
namespace PanelRuntime {
static uint8_t nextMember(uint8_t from){while(from<model.map.outputs&&!(groupMask&(1u<<from)))++from;return from;}
static void failGroup(const char *message){groupJob=0;gainBaseline.valid=false;savedKnown=false;showToast(confirmedTargets?"Partial - verify outputs":message);}
static void controlFinished(){
  if(copyJob.stage==2){
    if(control.completion!=Completion::Confirmed){copyJob.stage=0;showToast(control.completion==Completion::Unknown?"Copy unknown - verify target":"Copy partial - verify target");clearCells();return;}
    ++confirmedTargets;
    if(++copyJob.write>17){copyJob.stage=0;gainBaseline.valid=subBaseline.valid=balanceBaseline.valid=false;showToast("Parameters copied");clearCells();}return;
  }
  if(hardwareGuard==5){
    if(control.completion==Completion::Failed&&control.nativeStatus){hardwareGuard=hardwarePriorMute?0:7;resultToast();return;}
    hardwareGuard=6;settleUntil=millis()+120;return;}
  if(control.completion!=Completion::Confirmed){
    if(groupJob){failGroup(control.completion==Completion::Unknown?"Unknown - verify outputs":"Output change rejected");}
    else{resultToast();if(presetStage)presetStage=0;sumStage=0;linkedTarget=255;linkedSecond=false;}
    clearCells();return;
  }
  if(groupJob==1||groupJob==8||groupJob==6||groupJob==9){
    if(groupJob==8||groupJob==6){freshGains[groupAt]=control.value;gainKnownMask|=1u<<groupAt;}++confirmedTargets;++groupAt;return;
  }
  if(sumStage==3||sumStage==4||sumStage==5){++confirmedTargets;++groupAt;return;}
  if(presetStage==3){++cleanEffect;return;}
  if(presetStage==4){if(++cleanBand>=10){cleanBand=0;++cleanInput;}return;}
  if(linkedTarget!=255){uint8_t target=linkedTarget;linkedTarget=255;confirmedTargets=1;
    bool started=linkedCopy?control.startBandCopy(target,linkedBand,control.recipe):linkedXoEnable?control.startXoEnabled(target,linkedBand,linkedValue==0):control.startBand(target,linkedBand,linkedField,linkedValue);
    linkedSecond=started;if(!started)showToast("Partial - linked target unavailable");return;}
  if(linkedSecond){linkedSecond=false;showToast("Applied both channels");clearCells();return;}
  resultToast();clearCells();
}
static bool copyService(){
  if(!copyJob.stage)return false;
  if(copyJob.generation!=generation){copyJob.stage=0;showToast("Copy stopped - verify target");return true;}
  if(copyJob.stage==1){
    const uint8_t n=copyJob.read;
    uint16_t selector=copyJob.source;uint8_t request=n==84?0x75:n==85?0x79:0x77;uint16_t length=n==86?1:4;
    if(n<84){request=0x43;if(!PanelControls::eqSelector(model.map,model.map.inputs+copyJob.source,copyNativeBand(n/6),static_cast<PanelControls::ScalarField>(n%6),selector)){copyJob.stage=0;return true;}}
    Reply r=query(request,selector,length);
    if(!validReply(r,length)||copyJob.generation!=generation){copyJob.stage=0;showToast("Source unknown - copy cancelled");return true;}
    if(n<84){auto &b=copyJob.bands[n/6];switch(n%6){case 0:b.type=r.bytes[0];break;case 1:b.frequencyHz=PanelControlMenus::wireFloat(r.bytes);break;case 2:b.q=PanelControlMenus::wireFloat(r.bytes);break;case 3:b.gainDb=PanelControlMenus::wireFloat(r.bytes);break;case 4:b.bypass=r.bytes[0];break;case 5:b.qpX512=r.bytes[0]|(uint16_t(r.bytes[1])<<8);break;}}
    else if(n==84)copyJob.gain=PanelControlMenus::wireFloat(r.bytes);else if(n==85)copyJob.delay=PanelControlMenus::wireFloat(r.bytes);else copyJob.mute=r.bytes[0];
    if(++copyJob.read==87){
      bool valid=PanelControls::finite(copyJob.gain)&&copyJob.gain>=-60&&copyJob.gain<=0&&PanelControls::finite(copyJob.delay)&&copyJob.delay>=0&&copyJob.delay<=PanelControlMenus::maxDelayMs(model.map,copyJob.target,control.sampleRate)&&copyJob.mute<=1;
      for(unsigned i=0;i<14&&valid;++i){PanelControls::EqPacket packet{};valid=PanelControls::packEq(model.map,model.map.inputs+copyJob.target,copyNativeBand(i),copyJob.bands[i],control.sampleRate,packet)==PanelControls::Result::Ok;}
      if(!valid){copyJob.stage=0;showToast("Source outside panel range");}else{copyJob.stage=2;showToast("Copying - target guarded");}
    }return true;
  }
  const uint8_t step=copyJob.write,target=copyJob.target;bool started=false;
  if(step==0)started=control.startScalar(0x77,0x76,target,1,ValueKind::Byte);
  else if(step<=14)started=control.startBandCopy(model.map.inputs+target,copyNativeBand(step-1),copyJob.bands[step-1]);
  else if(step==15)started=control.startScalar(0x75,0x74,target,copyJob.gain,ValueKind::Float);
  else if(step==16)started=control.startScalar(0x79,0x78,target,copyJob.delay,ValueKind::Float);
  else if(step==17)started=control.startScalar(0x77,0x76,target,copyJob.mute,ValueKind::Byte);
  if(!started){copyJob.stage=0;showToast("Copy stopped - verify target");}return true;
}
static bool groupService(){
  if(!groupJob)return false;
  groupAt=nextMember(groupAt);
  if(groupAt>=model.map.outputs){
    const uint8_t job=groupJob;groupAt=0;
    if(job==1){groupJob=0;showToast("Applied");clearCells();return true;}
    if(job==2||job==3){
      gainBaseline=job==2?subBaseline:balanceBaseline;
      if(!gainBaseline.valid||gainBaseline.mask!=groupMask){
        if(PanelControls::captureBaseline(freshGains,model.map.outputs,groupMask,generation,gainBaseline)!=PanelControls::Result::Ok){failGroup("Gains unavailable");return true;}
        if(job==2)subTrim=0;else balance=0;
      }else if(PanelControls::checkBaseline(gainBaseline,freshGains,model.map.outputs,generation)!=PanelControls::Result::Ok){gainBaseline.valid=false;groupJob=0;showToast("Console gains changed - rebase");return true;}
      if(job==2)subBaseline=gainBaseline;else balanceBaseline=gainBaseline;
      groupJob=0;menuPage=PAGE_SPEAKERS;menuIndex=job==2?2:3;
      float value=job==2?(PanelControls::bitCount(groupMask)==1?freshGains[nextMember(0)]:subTrim):balance;
      openDecimal(value,job==2&&PanelControls::bitCount(groupMask)==1?-60:-24,job==2&&PanelControls::bitCount(groupMask)==1?0:24,2,"dB");return true;
    }
    if(job==4||job==5){gainBaseline=job==4?subBaseline:balanceBaseline;PanelControls::Result result=job==4?
        PanelControls::planSubGain(gainBaseline,freshGains,model.map.outputs,generation,localValue,plannedGains):
        PanelControls::planBalance(gainBaseline,freshGains,model.map.outputs,generation,model.roles.left,model.roles.right,localValue,plannedGains);
      if(result!=PanelControls::Result::Ok){failGroup("Gains changed - rebase");return true;}
      groupJob=8;confirmedTargets=0;return true;}
    if(job==8){groupJob=0;
      if(PanelControls::confirmGains(gainBaseline,plannedGains,freshGains,model.map.outputs,generation)!=PanelControls::Result::Ok){showToast("Partial - baseline invalid");return true;}
      if(editRow==2){subTrim=localValue;subBaseline=gainBaseline;}else{balance=localValue;balanceBaseline=gainBaseline;}showToast("Applied");clearCells();return true;}
    if(job==6){groupJob=9;return true;}
    if(job==9){groupJob=0;gainBaseline.valid=subBaseline.valid=balanceBaseline.valid=false;balance=subTrim=0;showToast("Saved speaker fields restored");clearCells();return true;}
    if(job==7){memcpy(savedGains,freshGains,sizeof(savedGains));memcpy(savedMutes,freshMutes,sizeof(savedMutes));savedMask=groupMask;savedKnown=true;savedEligible=false;groupJob=0;return true;}
  }
  if(groupJob==1||groupJob==8||groupJob==6||groupJob==9){
    bool started=groupJob==1?control.startScalar(0x77,0x76,groupAt,localValue,ValueKind::Byte):
      groupJob==9?control.startScalar(0x77,0x76,groupAt,savedMutes[groupAt],ValueKind::Byte):
      control.startScalar(0x75,0x74,groupAt,plannedGains[groupAt],ValueKind::Float);
    if(!started)failGroup("Outside panel range - Console");return true;
  }
  const bool mute=groupJob==7&&roleCaptureMute;
  Reply r=query(mute?0x77:0x75,groupAt,mute?1:4);
  if(!validReply(r,mute?1:4)){failGroup("Unknown speaker fields");savedEligible=false;return true;}
  if(mute){freshMutes[groupAt]=r.bytes[0];roleCaptureMute=false;++groupAt;}
  else {freshGains[groupAt]=PanelControlMenus::wireFloat(r.bytes);if(!PanelControls::finite(freshGains[groupAt])){failGroup("Unknown gains");return true;}gainKnownMask|=1u<<groupAt;
    if(groupJob==7)roleCaptureMute=true;else ++groupAt;}
  return true;
}
static bool sumDraftService(){
  if(!sumContext()||sumAttempted==3)return false;
  const uint8_t row=(sumAttempted&1)?1:0,input=sumPair*2+row;
  Reply r=query(0x71,(uint16_t(input)<<8)|sumOutput,8);
  if(!sumContext())return true;sumAttempted|=1u<<row;
  if(validReply(r,8)&&r.bytes[0]==input&&r.bytes[1]==sumOutput&&r.bytes[2]<=1&&r.bytes[3]<=1){
    float gain=PanelControlMenus::wireFloat(r.bytes+4);
    if(PanelControls::finite(gain)){sumOriginal[row]={input,sumOutput,r.bytes[2]!=0,r.bytes[3]!=0,gain};sumGains[row]=gain;sumKnown|=1u<<row;}}
  return true;
}
static bool sumService(){
  if(!sumStage)return false;
  if(!sumReady()){sumStage=0;showToast("Routes changed - Sum stopped");return true;}
  if(groupAt>=model.map.inputs){
    groupAt=0;
    if(sumStage==1){sumStage=2;return true;}
    if(sumStage==3){sumStage=4;return true;}
    if(sumStage==4){sumStage=5;return true;}
    if(sumStage==5){sumStage=0;showToast("Routing replaced");clearCells();return true;}
  }
  if(sumStage==1){Reply r=query(0x71,(uint16_t(groupAt)<<8)|model.output,8);
    if(!validReply(r,8)||r.bytes[0]!=groupAt||r.bytes[1]!=sumOutput||r.bytes[2]>1||r.bytes[3]>1||!PanelControls::finite(PanelControlMenus::wireFloat(r.bytes+4))){sumStage=0;showToast("Unknown route - unchanged");return true;}
    sumRoutes[groupAt]={r.bytes[0],r.bytes[1],r.bytes[2]!=0,r.bytes[3]!=0,PanelControlMenus::wireFloat(r.bytes+4)};
    if(groupAt==sumPair*2||groupAt==sumPair*2+1){const auto &old=sumOriginal[groupAt-sumPair*2];const auto &fresh=sumRoutes[groupAt];if(old.enabled!=fresh.enabled||old.invert!=fresh.invert||old.gainDb!=fresh.gainDb){sumStage=0;showToast("Route changed - reopen Sum");return true;}}
    ++groupAt;return true;}
  if(sumStage==2){sumStage=3;groupAt=0;return true;}
  // Each route is read again by Controller immediately before this field
  // update. Disable first, set gains while silent, then enable selected routes.
  // Both user-selected gains are verified before either route is enabled.
  const uint8_t left=sumPair*2,right=left+1;
  bool selected=groupAt==left||groupAt==right;
  if((sumStage==4||sumStage==5)&&!selected){++groupAt;return true;}
  if(!control.startRoute(groupAt,model.output,sumStage==4?PanelControls::RouteField::Gain:PanelControls::RouteField::Enabled,
      sumStage==4?sumGains[groupAt-left]:(sumStage==5?1:0))){sumStage=0;showToast("Route unavailable");}
  return true;
}
static bool hardwareService(){
  if(!hardwareGuard)return false;
  if(hardwareGeneration!=generation){hardwareGuard=0;showToast("Context changed - mute not restored");return true;}
  if(hardwareGuard==1){Reply r=query(REQ_GET_USER_MUTE,0,1);if(!validReply(r,1)||r.bytes[0]>1){hardwareGuard=0;showToast("Mute unknown - unchanged");return true;}hardwarePriorMute=r.bytes[0]!=0;hardwareGuard=2;return true;}
  if(hardwareGuard==2||hardwareGuard==7){Frame f;f.get=false;f.request=REQ_SET_USER_MUTE;f.length=1;f.bytes[0]=hardwareGuard==2?1:hardwarePriorMute;
    Reply r=exchange(nullptr,f);hardwareGuard=hardwareGuard==2?3:8;if(r.status==WireStatus::Rejected){hardwareGuard=0;showToast("Mute rejected - unchanged");}return true;}
  if(hardwareGuard==3||hardwareGuard==8){const bool restoring=hardwareGuard==8;Reply r=query(REQ_GET_USER_MUTE,0,1);
    if(!validReply(r,1)||r.bytes[0]!=(restoring?uint8_t(hardwarePriorMute):1)){hardwareGuard=0;showToast("Mute unknown - verify state");return true;}
    dspi.muted=r.bytes[0]!=0;
    if(restoring){hardwareGuard=0;resultToast();clearCells();return true;}
    if(hardwareCancel){hardwareGuard=hardwarePriorMute?0:7;showToast("Cancelled - hardware unchanged");return true;}
    hardwareGuard=4;return true;}
  if(hardwareGuard==4){
    if(hardwareCancel){hardwareGuard=hardwarePriorMute?0:7;showToast("Cancelled - hardware unchanged");return true;}readCell(editPage,editRow);
    if(hardwareGeneration!=generation){hardwareGuard=0;return true;}
    if(!cells[editRow].known||!PanelControlMenus::near(cells[editRow].value,originalValue)){hardwareGuard=hardwarePriorMute?0:7;showToast("Changed - hardware unchanged");return true;}
    hardwareGuard=5;
    if(!startFieldWrite(editPage,editRow,localValue)){hardwareGuard=hardwarePriorMute?0:7;return true;}
    if(!control.busy()){hardwareGuard=6;settleUntil=millis()+120;}return true;}
  if(hardwareGuard==5)return true; // Controller is serviced before this branch.
  if(hardwareGuard==6){if((long)(millis()-settleUntil)<0)return true;
    readCell(editPage,editRow);++hardwareReadbacks;
    if(cells[editRow].known&&PanelControlMenus::near(cells[editRow].value,localValue)){
      const Field f=field(editPage,editRow);
      if(f.pin||f.set==0xed){hardwareGuard=0;showToast(f.set==0xed?"Selected rate - still muted":"Requested - still muted");return true;}
      control.completion=Completion::Confirmed;hardwareGuard=hardwarePriorMute?0:7;if(!hardwareGuard)showToast("Requested");return true;
    }
    if(hardwareReadbacks<3){settleUntil=millis()+150;return true;}
    hardwareGuard=0;showToast("Unknown - left muted");return true;}
  return true;
}
static bool nameService(){
  if(!nameStage)return false;
  const uint8_t id=nameJobTarget;const char *draft=nameJobValue;
  if(nameStage==1){Reply r=query(0x9c,id,32);if(!validReply(r,32)){nameStage=0;showToast("Name unknown - unchanged");return true;}
    size_t n=0;while(n<32&&r.bytes[n])++n;if(n>31||!PanelControls::validStoredName(reinterpret_cast<char*>(r.bytes),n)){nameStage=0;showToast("Native name needs Console");return true;}nameStage=2;return true;}
  if(nameStage==2){Frame f;f.get=false;f.request=0x9b;f.value=id;f.length=strlen(draft)+1;memcpy(f.bytes,draft,f.length);Reply r=exchange(nullptr,f);nameStage=3;if(r.status==WireStatus::Rejected){nameStage=0;showToast("Name rejected");}return true;}
  Reply r=query(0x9c,id,32);nameStage=0;
  if(validReply(r,32)&&memcmp(r.bytes,draft,strlen(draft)+1)==0){strncpy(channelNames[id],draft,32);namesKnown[id]=true;if(nameReturnAllowed)enterPage(PAGE_OUTPUT);showToast("Name confirmed");}
  else showToast("Name unknown - verify");return true;
}
static bool presetService(){
  if(!presetStage)return false;
  const ConfirmAction confirmation=presetJobAction;const uint8_t presetSlot=presetJobSlot;const char *presetNameDraft=presetJobName;
  if(presetJobGeneration!=generation&&presetStage<12){presetStage=saveAttempted?12:renameAttempted?16:0;showToast("Context changed - preset cancelled");return true;}
  if(presetStage==1){if(!directoryRead()){presetStage=0;showToast("Unknown directory - no save");return true;}
    PanelControls::PresetDirectory d{occupiedMask,generation,true};PanelControls::PresetPlan p{};
    const auto action=confirmation==ConfirmAction::Rename?PanelControls::PresetAction::Rename:
        confirmation==ConfirmAction::Save?PanelControls::PresetAction::SaveChanges:PanelControls::PresetAction::SaveAsNew;
    if(PanelControls::planPreset(action,presetSlot,dspi.activePreset,d,generation,true,p)!=PanelControls::Result::Ok){presetStage=0;showToast("Destination changed - cancelled");return true;}
    presetWasEmpty=(occupiedMask&(1u<<presetSlot))==0;
    presetStage=confirmation==ConfirmAction::Rename?7:14;return true;
  }
  if(presetStage==3){static const uint8_t gets[]={0x59,0x5f,0xb5,0x31,0x11,0x3f};static const uint8_t sets[]={0x58,0x5e,0xb4,0x30,0x10,0x3e};
    if(cleanEffect>=6){presetStage=4;cleanInput=cleanBand=0;return true;}
    if(!control.startScalar(gets[cleanEffect],sets[cleanEffect],0,0,cleanEffect==5?ValueKind::Float:ValueKind::Byte)){presetStage=0;showToast("Reset unavailable - not saved");}return true;
  }
  if(presetStage==4){if(cleanInput>=model.map.inputs){presetStage=5;return true;}
    if(!control.startBand(cleanInput,cleanBand,PanelControls::BandField::Bypass,1)){presetStage=0;showToast("Input EQ reset unavailable");}return true;
  }
  if(presetStage==5){
    // Recheck empty occupancy immediately before the single action, including
    // after the optional reset sequence. No destination load/defaults command.
    if(confirmation!=ConfirmAction::Save){if(!directoryRead()||(occupiedMask&(1u<<presetSlot))){presetStage=0;showToast("Destination occupied - not saved");return true;}presetStage=6;return true;}
    presetStage=6;return true;
  }
  if(presetStage==6){Frame f;f.request=0x90;f.value=presetSlot;f.length=1;f.action=true;saveAttempted=true;Reply r=exchange(nullptr,f);
    if(presetJobGeneration!=generation){presetStage=12;return true;}
    saveAckKnown=validReply(r,1)&&r.bytes[0]==0;
    if(r.status==WireStatus::Rejected||(validReply(r,1)&&r.bytes[0]!=0)){presetStage=0;showToast("Save rejected");return true;}
    statusText=saveAckKnown?"Save accepted":"Save unknown - resolving";presetStage=10;settleUntil=millis()+PRESET_SAVE_REFRESH_MS;return true;
  }
  if(presetStage==7){if((long)(millis()-settleUntil)<0)return true;
    if(confirmation==ConfirmAction::Save||presetCancelled){presetStage=9;return true;}
    Frame f;f.get=false;f.request=0x94;f.value=presetSlot;f.length=strlen(presetNameDraft)+1;memcpy(f.bytes,presetNameDraft,f.length);renameAttempted=true;Reply r=exchange(nullptr,f);
    if(r.status==WireStatus::Rejected){presetStage=0;showToast("Rename rejected / save accepted");return true;}
    presetStage=8;settleUntil=millis()+PRESET_SAVE_REFRESH_MS;return true;
  }
  if(presetStage==8){if((long)(millis()-settleUntil)<0)return true;Reply r=query(0x93,presetSlot,32);
    if(!validReply(r,32)||memcmp(r.bytes,presetNameDraft,strlen(presetNameDraft)+1)!=0){presetStage=0;showToast("Name unknown - do not repeat");return true;}
    strncpy(dspi.presetNames[presetSlot],presetNameDraft,32);presetStage=9;return true;
  }
  if(presetStage==9){if(!directoryRead()||!(occupiedMask&(1u<<presetSlot))){presetStage=0;showToast("Accepted - persistence unknown");return true;}
    dspi.occupiedPresets=occupiedMask;presetStage=0;String result=confirmation==ConfirmAction::Rename?String("Name confirmed - durability unknown"):String(statusText);if(!presetCancelled)enterPage(PAGE_PRESET_TOOLS);showToast(result);return true;}
  if(presetStage==10){if((long)(millis()-settleUntil)<0)return true;
    if(!directoryRead()||!(occupiedMask&(1u<<presetSlot))){presetStage=0;showToast("Accepted - persistence unknown");return true;}presetStage=11;return true;}
  if(presetStage==11){Reply r=query(0x9a,0,1);if(!validReply(r,1)||r.bytes[0]!=presetSlot){presetStage=0;showToast("Accepted - active slot unknown");return true;}
    if(!saveAckKnown&&!presetWasEmpty&&presetActiveBefore==presetSlot){presetStage=0;savedKnown=false;showToast("Save unknown - panel snapshot held");return true;}
    bool panelOk=persistPresetSnapshot(presetSlot);statusText=panelOk?(saveAckKnown?"Save accepted":"Save observed - durability unknown"):"Accepted - panel storage error";
    dspi.activePreset=presetSlot;roleSlot=presetSlot;model.roles=presetSnapshot.roles;gainBaseline.valid=subBaseline.valid=balanceBaseline.valid=false;savedEligible=false;
    savedKnown=presetSnapshot.speakerKnown&&memcmp(serial,presetSnapshot.serial,17)==0;savedMask=presetSnapshot.speakerMask;savedSlot=presetSlot;memcpy(savedSerial,serial,17);memcpy(savedGains,presetSnapshot.gains,sizeof(savedGains));memcpy(savedMutes,presetSnapshot.mutes,sizeof(savedMutes));
    presetStage=7;return true;}
  if(presetStage==12){directoryRead();presetStage=13;return true;}
  if(presetStage==13){query(0x9a,0,1);presetStage=0;showToast("Context changed - save unknown");return true;}
  if(presetStage==16){query(0x93,presetSlot,32);presetStage=0;showToast("Context changed - rename unknown");return true;}
  if(presetStage==14||presetStage==15){
    if(!presetSnapshot.speakerKnown){presetStage=confirmation==ConfirmAction::New&&effectsOff?3:5;return true;}
    while(captureOutput<model.map.outputs&&!(presetSnapshot.speakerMask&(1u<<captureOutput)))++captureOutput;
    if(captureOutput>=model.map.outputs){presetStage=confirmation==ConfirmAction::New&&effectsOff?3:5;return true;}
    bool mute=presetStage==15;Reply r=query(mute?0x77:0x75,captureOutput,mute?1:4);
    if(presetJobGeneration!=generation)return true;
    if(!validReply(r,mute?1:4)){presetSnapshot.speakerKnown=false;presetStage=14;return true;}
    if(mute){presetSnapshot.mutes[captureOutput]=r.bytes[0];if(r.bytes[0]>1)presetSnapshot.speakerKnown=false;++captureOutput;presetStage=14;}
    else{float gain=PanelControlMenus::wireFloat(r.bytes);presetSnapshot.gains[captureOutput]=gain;if(PanelControls::outputGainRange(gain)!=PanelControls::Result::Ok)presetSnapshot.speakerKnown=false;presetStage=15;}return true;
  }
  return true;
}
}
void servicePanelControls(){using namespace PanelRuntime;
  // The ordinary loop calls this after the unchanged media services, including
  // while playback is active; reserve/seek/transition and exclusive guards win.
  static bool wasConnected=false;
  if(!dspi.connected){if(wasConnected){panelCancel();control=PanelControlMenus::Controller(exchange,nullptr);control.completion=Completion::Unknown;hardwareGuard=groupJob=sumStage=presetStage=nameStage=0;identityAttempted=serialKnown=capabilityAttempted=uartKnown=uartAttempted=false;capabilityStep=activeInputStep=0;evidence=PanelControls::CapabilityEvidence();platformByte=255;reportedOutputs=0;model.map=PanelControls::ChannelMap();model.roles=PanelControlMenus::Roles();aliasesDirty=false;roleSlot=255;panelInvalidate();}wasConnected=false;return;}wasConnected=true;
  if(wifiTransferExclusive()||dspiRequestInFlight||
      mediaTrackTransitionActive()||mediaPlayerPoc.seeking()||mediaPlaybackBufferLow()||
      presetSaveCompletionPending||mediaPresetRefreshPending)return;
#if DSPI_HAVE_NIMBLE
  if(bleConnectTaskRunning||bleRemovalInProgress||uiView==VIEW_BLE)return;
#endif
  const bool visible=panelOwnsPage(menuPage)&&(uiView==VIEW_MENU||uiView==VIEW_EDIT);
  if(externalChangeNotice){externalChangeNotice=false;if(visible){showToast("Changed externally - read again");drawMenu();}}
  if(!identityAttempted){identityAttempted=true;Reply r=query(0x7e,0,16);serialKnown=validReply(r,16);
    if(serialKnown){for(unsigned i=0;i<16;++i){if(r.bytes[i]<33||r.bytes[i]>126)serialKnown=false;serial[i]=r.bytes[i];}serial[16]=0;}
    if(serialKnown)loadLocal();if(visible)drawMenu();return;}
  if(!visible&&!control.busy()&&!hardwareGuard&&!nameStage&&!presetStage&&!groupJob&&!sumStage&&!linkStage&&!copyJob.stage)return;
  control.sampleRate=dspi.sampleRate;
  if(control.busy()){control.service();if(!control.busy())controlFinished();if(visible)drawMenu();return;}
  if(copyService()||linkService()||hardwareService()||nameService()||groupService()||sumService()||presetService()){if(visible)drawMenu();return;}
  if(!visible||editActive)return;
  if(!capabilityAttempted){capabilityService();drawMenu();return;}
  if(roleSlot!=dspi.activePreset)panelPresetChanged(dspi.activePreset);
  if(!activeInputStep&&model.map.writable){Reply r=query(0x50,23,4);activeInputStep=1;if(validReply(r,4)){uint32_t n;memcpy(&n,r.bytes,4);if(n>0&&n<=model.map.inputs)model.activeInputs=n;}drawMenu();return;}
  if(menuPage>=PAGE_HARDWARE&&!uartAttempted){uartAttempted=true;Reply r=query(0xf6,0,8);uartKnown=validReply(r,8)&&r.bytes[0]==1;if(uartKnown){uartTx=r.bytes[1];uartRx=r.bytes[2];}drawMenu();return;}
  if(!model.map.writable)return;
  if(menuPage==PAGE_SUM){if(sumDraftService())drawMenu();return;}
  if(menuPage==PAGE_EQ_BAND||menuPage==PAGE_XO_BAND){if(bandService())drawMenu();return;}
  if(menuPage==PAGE_PRESET_DESTINATION&&!directoryKnown){directoryRead();drawMenu();return;}
  const uint8_t first=(menuIndex/4)*4,count=panelMenuCount(menuPage),end=std::min<uint8_t>(first+4,count);
  if(cacheFirst!=first){cacheFirst=first;for(unsigned i=first;i<end&&i<12;++i)cells[i].attempted=cells[i].known=false;}
  if(menuPage==PAGE_OUTPUT){if(readName(model.map.inputs+model.output)){drawMenu();return;}}
  if(menuPage==PAGE_OUTPUTS||menuPage==PAGE_SPEAKER_MEMBERS||menuPage==PAGE_EQ_OUTPUTS||menuPage==PAGE_COPY_DESTINATION){for(unsigned i=first;i<end&&i<model.map.outputs;++i)if(readName(model.map.inputs+i)){drawMenu();return;}}
  if(menuPage==PAGE_EQ_INPUTS||menuPage==PAGE_ROUTING){for(unsigned i=first;i<end&&i<model.visibleInputs();++i)if(readName(i)){drawMenu();return;}}
  if(menuPage==PAGE_SPEAKERS){uint16_t mask=model.roles.left|model.roles.right|model.roles.subs;
    for(unsigned i=0;i<model.map.outputs;++i)if((mask&(1u<<i))&&!cells[i].attempted){Reply r=query(0x77,i,1);cells[i].attempted=true;cells[i].known=validReply(r,1);if(cells[i].known)cells[i].value=r.bytes[0];drawMenu();return;}
    if(first==0&&PanelControls::bitCount(model.roles.subs)==1){uint8_t sub=__builtin_ctz(model.roles.subs);if(!(gainKnownMask&(1u<<sub))&&!(gainAttemptedMask&(1u<<sub))){gainAttemptedMask|=1u<<sub;Reply r=query(0x75,sub,4);if(validReply(r,4)){freshGains[sub]=PanelControlMenus::wireFloat(r.bytes);if(PanelControls::finite(freshGains[sub]))gainKnownMask|=1u<<sub;}drawMenu();return;}}return;}
  for(uint8_t i=first;i<end&&i<12;++i)if(!cells[i].attempted&&field(menuPage,i).get){readCell(menuPage,i);drawMenu();return;}
}
