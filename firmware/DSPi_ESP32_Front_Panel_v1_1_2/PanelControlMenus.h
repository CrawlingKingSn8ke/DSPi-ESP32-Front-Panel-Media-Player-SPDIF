#ifndef DSPI_PANEL_CONTROL_MENUS_H
#define DSPI_PANEL_CONTROL_MENUS_H
#include "PanelControls.h"
// Fixed-capacity, host-testable menu and transaction policy. The adapter owns
// UART arbitration. Each service() invokes the wire at most once; no timers,
// full-state sweeps, audio calls, allocation, or persistence live here.
namespace PanelControlMenus {
enum class Page : uint8_t { Main, Input, Music, Preset, Effects, System,
  Speakers, Setup, Groups, GroupMembers, Outputs, Output, Routing, Route,
  Sum, SourcePair, Equaliser, EqInputs, EqOutputs, EqTarget, Bands, Band,
  Crossover, XoBand, PresetTools, PresetDestination, PresetPreview,
  StartingPoint, InputNames, InputName, Hardware, HardwareOutputs,
  HardwarePair, Pdm, SpdifInputs, SpdifInput, I2sInput, I2sData,
  I2sClocks, MasterClock, Adat, Confirm, Name, Number, CopyDestination, CopyPreview, Count };
enum class Feature : uint8_t { Equaliser, Loudness, Crossfeed, Leveller,
  PsyBass, SubSynth, Tube, Limiter };
inline uint8_t mainCount(){return 6;}
inline const char *mainLabel(uint8_t row){static const char *v[]={"Input","Music","Preset","Outputs","Effects","System"};return v[row<6?row:5];}
inline Feature featureSemantic(uint8_t row){return static_cast<Feature>(row<8?row:7);}
inline uint8_t featureMask(Feature f){return f==Feature::Equaliser?0:static_cast<uint8_t>(1u<<(static_cast<uint8_t>(f)-1));}
inline bool safeAdjust(float current,int direction,float step,float minimum,float maximum,float &out){
  if(!PanelControls::finite(current)||current<minimum||current>maximum)return false;
  float next=current+(direction>0?step:-step);if(next<minimum)next=minimum;if(next>maximum)next=maximum;out=next;return true;
}
inline float maxDelayMs(const PanelControls::ChannelMap &map,uint8_t output,float rate){
  if(!map.writable||!PanelControls::knownMap(map)||output>=map.outputs||!PanelControls::finite(rate)||rate<=0)return 0;
  const unsigned samples=(map.outputs==9?2047u:1023u)-(output==map.outputs-1?128u:0u);
  return samples*1000.0f/rate;
}
struct Roles { uint16_t left=0,right=0,subs=0; };
struct Model {
  PanelControls::ChannelMap map{}; Roles roles;
  Page eqParent=Page::Equaliser,confirmParent=Page::Speakers,nameParent=Page::InputName;
  uint8_t target=0,output=0,input=0,band=0,group=0,pair=0,source=0,activeInputs=0;
  uint8_t visibleInputs()const{return activeInputs&&activeInputs<=map.inputs?activeInputs:map.inputs;}
  bool eqOutput=false,link=false;
  uint8_t count(Page p)const {
    switch(p){
      case Page::Main:return 6;case Page::Speakers:return 4;case Page::Setup:return 2;
      case Page::Outputs:return (map.outputs?map.outputs:1)+2;
      case Page::Groups:return 3;case Page::GroupMembers:case Page::EqOutputs:case Page::CopyDestination:return map.outputs?map.outputs:1;
      case Page::Output:return 8;case Page::Routing:return visibleInputs()?visibleInputs()+1:1;
      case Page::Route:return 3;case Page::Sum:return 3;case Page::SourcePair:return visibleInputs()>=2?visibleInputs()/2:1;
      case Page::Equaliser:return 2;case Page::EqInputs:return visibleInputs()?visibleInputs():1;
      case Page::EqTarget:return eqOutput?2:1;case Page::Bands:return 10;case Page::Band:return 5;
      case Page::Crossover:return 4;case Page::XoBand:return 5;case Page::PresetTools:return 5;
      case Page::PresetDestination:return 10;case Page::PresetPreview:return 7;case Page::StartingPoint:return 2;
      case Page::InputNames:return 8;case Page::InputName:return 5;case Page::Hardware:return 8;
      case Page::HardwareOutputs:return map.stereoPairs?map.stereoPairs:1;case Page::HardwarePair:return 2;
      case Page::Pdm:return 1;case Page::SpdifInputs:return 4;case Page::SpdifInput:return 2;
      case Page::I2sInput:return 2;case Page::I2sData:return map.inputs>=2?map.inputs/2:1;
      case Page::I2sClocks:return 6;case Page::MasterClock:return 3;case Page::Adat:return 7;
      case Page::CopyPreview:return 6;
      case Page::Confirm:return 4;case Page::Name:case Page::Number:return 4;default:return 1;
    }
  }
  uint8_t nativeBand(uint8_t row,bool xo)const{return static_cast<uint8_t>(row+(xo?20:0));}
  bool assignRole(uint8_t role,uint8_t out,bool enabled){
    if(role>2||out>=map.outputs)return false;uint16_t bit=uint16_t(1)<<out;
    uint16_t &v=role==0?roles.left:role==1?roles.right:roles.subs;
    if(enabled&&((roles.left|roles.right|roles.subs)&bit)&&!(v&bit))return false;
    if(enabled)v|=bit;else v&=~bit;return true;
  }
  Page parent(Page p)const{
    switch(p){case Page::Outputs:return Page::Main;case Page::Setup:case Page::Speakers:return Page::Outputs;case Page::Groups:return Page::Setup;
      case Page::GroupMembers:return Page::Groups;case Page::Output:return Page::Outputs;case Page::Routing:return Page::Output;case Page::Crossover:return Page::EqTarget;
      case Page::Route:case Page::SourcePair:return Page::Routing;case Page::Sum:return visibleInputs()>2?Page::SourcePair:Page::Routing;
      case Page::EqInputs:return Page::Effects;case Page::EqOutputs:return Page::Equaliser;case Page::EqTarget:return eqParent;
      case Page::CopyDestination:return Page::Output;case Page::CopyPreview:return Page::CopyDestination;
      case Page::Bands:return Page::EqTarget;case Page::Band:return Page::Bands;case Page::XoBand:return Page::Crossover;
      case Page::Equaliser:return Page::Effects;case Page::PresetTools:return Page::Preset;
      case Page::PresetDestination:case Page::PresetPreview:return Page::PresetTools;case Page::StartingPoint:return Page::PresetPreview;
      case Page::InputName:return Page::InputNames;case Page::InputNames:case Page::Hardware:return Page::System;
      case Page::HardwareOutputs:case Page::Pdm:case Page::SpdifInputs:case Page::I2sInput:case Page::I2sClocks:case Page::MasterClock:case Page::Adat:return Page::Hardware;
      case Page::HardwarePair:return Page::HardwareOutputs;case Page::SpdifInput:return Page::SpdifInputs;case Page::I2sData:return Page::I2sInput;
      case Page::Confirm:return confirmParent;case Page::Name:return nameParent;default:return Page::Main;}
  }
};
struct NameEditor {
  char text[32]{};uint8_t cursor=0,limit=31;bool active=false;
  static const char *characters(){return " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-/+";}
  bool begin(const char *s,uint8_t max){if(!s||max==0||max>31||strlen(s)>max)return false;
    for(size_t i=0;s[i];++i)if(!strchr(characters(),s[i]))return false;
    memset(text,0,sizeof(text));memcpy(text,s,strlen(s));limit=max;cursor=0;active=true;return true;}
  void move(int d){if(active){int p=int(cursor)+d;cursor=static_cast<uint8_t>(p<0?0:p>=limit?limit-1:p);}}
  void adjust(int d){if(!active)return;size_t n=strlen(text);while(n<=cursor)text[n++]=' ';text[n]=0;
    const char *a=characters(),*p=strchr(a,text[cursor]);int at=p?int(p-a):0,size=int(strlen(a));at=(at+(d>0?1:-1)+size)%size;text[cursor]=a[at];}
  bool preview(char *out,size_t capacity,uint16_t pixels,PanelControls::MeasureText measure,void *context,uint16_t &offset,uint16_t &width)const{
    if(!out||capacity<2||!measure||cursor>=limit)return false;
    size_t length=strlen(text),last=length>cursor?length:cursor+1;
    size_t first=cursor>4?cursor-4:0,end=first+10<last?first+10:last;
    for(;;){size_t n=0;for(size_t i=first;i<end&&n+1<capacity;++i)out[n++]=i<length?text[i]:' ';out[n]=0;
      if(measure(out,context)<=pixels)break;
      if(end-first<=1)return false;
      if(cursor-first>end-cursor-1)++first;else --end;
    }
    char prefix[32]{};size_t count=cursor-first;memcpy(prefix,out,count);offset=measure(prefix,context);
    char selected[2]={cursor<length?text[cursor]:' ',0};width=measure(selected,context);if(!width)width=1;return true;
  }
  bool save(){if(!active)return false;size_t n=strlen(text);while(n&&text[n-1]==' ')text[--n]=0;
    if(!PanelControls::validStoredName(text,n))return false;active=false;return true;}
  void cancel(){active=false;}
};
enum class WireStatus:uint8_t {Ok,Rejected,Unknown};
struct Frame { bool get=true;uint8_t request=0;uint16_t value=0,length=0;uint8_t bytes[32]{};bool action=false; };
struct Reply {WireStatus status=WireStatus::Unknown;uint8_t size=0,bytes[64]{};};
typedef Reply(*Wire)(void *,const Frame &);
enum class ValueKind:uint8_t {Byte,Float,Word,Dword};
inline float wireFloat(const uint8_t *p){float v;memcpy(&v,p,4);return v;}
inline bool near(float a,float b){return PanelControls::finite(a)&&PanelControls::finite(b)&&fabsf(a-b)<0.002f;}
class Controller {
 public:
  PanelControls::ChannelMap map{};float sampleRate=48000;
  PanelControls::Completion completion=PanelControls::Completion::Pending;
  PanelControls::BandRecipe recipe{};PanelControls::Route route{};
  float value=0;uint8_t nativeStatus=0;
  uint8_t readBytes=0,readOffset=0,readMask=0;
  Controller(Wire fn,void *ctx):wire(fn),context(ctx){}
  bool busy()const{return phase!=Idle;}
  bool sent()const{return didWrite;}
  void invalidate(){++generation;if(busy()&&!didWrite)invalid=true;}
  void cancel(){if(!didWrite){phase=Idle;completion=PanelControls::Completion::Failed;}else cancelled=true;}
  bool startScalar(uint8_t get,uint8_t set,uint16_t selector,float desired,ValueKind k){
    if(!validScalar(get,set,selector,desired,k))return false;
    if(!begin(Scalar,desired))return false;getRequest=get;setRequest=set;valueSelector=selector;kind=k;return true;}
  bool startBand(uint8_t eq,uint8_t b,PanelControls::BandField f,float desired){
    if(!PanelControls::bandTarget(map,eq,b)||!map.writable||!begin(Eq,desired))return false;
    eqId=eq;band=b;bandField=f;return true;}
  bool startXoEnabled(uint8_t eq,uint8_t b,bool enabled){
    if(b<20||b>23||eq<map.inputs||!startBand(eq,b,PanelControls::BandField::Bypass,enabled?0:1))return false;
    createXoIfFlat=enabled;return true;
  }
  // Only the explicit per-band copy confirmation may call this path.
  bool startBandCopy(uint8_t eq,uint8_t b,const PanelControls::BandRecipe &source){
    PanelControls::EqPacket packet{};
    if(PanelControls::packEq(map,eq,b,source,sampleRate,packet)!=PanelControls::Result::Ok||!startBand(eq,b,PanelControls::BandField::Type,source.type))return false;
    copyRecipe=source;copyBand=true;return true;
  }
  bool startRoute(uint8_t in,uint8_t out,PanelControls::RouteField f,float desired){
    uint16_t s;if(!PanelControls::routeSelector(map,in,out,s)||!begin(Route,desired))return false;
    valueSelector=s;routeField=f;return true;}
  // Native action setters are GET-framed wValue commands with one status byte.
  // No replay on ambiguous transport completion. Readback means requested state,
  // never physical pin activity or flash durability.
  bool startAction(uint8_t set,uint8_t get,uint16_t selector,float desired,ValueKind k,uint16_t readSelector=0xffff){
    if(!begin(Action,desired))return false;setRequest=set;getRequest=get;valueSelector=selector;readValue=readSelector==0xffff?selector:readSelector;kind=k;phase=Write;return true;}
  void service(){
    if(phase==Idle)return;
    if(invalid&&!didWrite){finish(PanelControls::Completion::Failed);return;}
    Frame f;f.get=true;
    if(operation==Eq){
      if(phase==Read||phase==Verify){uint16_t s=0;if(!PanelControls::eqSelector(map,eqId,band,static_cast<PanelControls::ScalarField>(field),s)){finish(PanelControls::Completion::Failed);return;}f.request=0x43;f.value=s;f.length=4;}
      else {PanelControls::EqPacket p{};if(PanelControls::packEq(map,eqId,band,planned,sampleRate,p)!=PanelControls::Result::Ok){finish(PanelControls::Completion::Failed);return;}f.get=false;f.request=0x42;f.length=p.size;memcpy(f.bytes,p.bytes,p.size);}
    }else if(operation==Route){f.request=phase==Write?0x70:0x71;f.value=valueSelector;f.length=8;
      if(phase==Write){PanelControls::RoutePacket p{};if(PanelControls::packRoute(map,plannedRoute,p)!=PanelControls::Result::Ok){finish(PanelControls::Completion::Failed);return;}f.get=false;memcpy(f.bytes,p.bytes,8);}
    }else {f.request=phase==Write?setRequest:getRequest;f.value=operation==Action&&phase==Verify?readValue:valueSelector;f.length=kind==ValueKind::Float||kind==ValueKind::Dword?4:kind==ValueKind::Word?2:1;
      if(phase!=Write&&readBytes)f.length=readBytes;
      if(phase==Write){if(operation==Action){f.get=true;f.action=true;f.length=1;}else{f.get=false;if(kind==ValueKind::Float)PanelControls::putFloatLE(f.bytes,desiredValue);else{uint32_t integer=static_cast<uint32_t>(desiredValue);for(unsigned i=0;i<f.length;++i)f.bytes[i]=integer>>(8*i);}}}}
    const bool mutation=phase==Write;
    if(mutation)didWrite=true; // attempted can mean applied even when ACK is lost
    Reply r=wire(context,f);
    if(r.status!=WireStatus::Ok){
      if(mutation&&r.status==WireStatus::Unknown){completion=PanelControls::Completion::Unknown;phase=Verify;field=0;return;}
      finish(r.status==WireStatus::Unknown?PanelControls::Completion::Unknown:PanelControls::Completion::Failed);return;}
    if(phase==Write){didWrite=true;if(operation==Action){if(r.size!=1){finish(PanelControls::Completion::Unknown);return;}nativeStatus=r.bytes[0];if(nativeStatus){finish(PanelControls::Completion::Failed);return;}}phase=Verify;field=0;return;}
    if(operation==Eq){if(r.size!=4){finish(PanelControls::Completion::Unknown);return;}PanelControls::BandRecipe &b=phase==Read?recipe:readback;
      switch(field){case 0:b.type=r.bytes[0];break;case 1:b.frequencyHz=wireFloat(r.bytes);break;case 2:b.q=wireFloat(r.bytes);break;case 3:b.gainDb=wireFloat(r.bytes);break;case 4:b.bypass=r.bytes[0];break;case 5:b.qpX512=r.bytes[0]|(uint16_t(r.bytes[1])<<8);break;}
      if(++field<6)return;field=0;
      if(phase==Read){PanelControls::BandRecipe fresh=recipe;
        if(createXoIfFlat&&fresh.type==0&&PanelControls::editBand(recipe,PanelControls::BandField::Type,32,sampleRate,fresh)!=PanelControls::Result::Ok){finish(PanelControls::Completion::Failed);return;}
        if(copyBand){planned=copyRecipe;if(planned.type!=11)planned.qpX512=fresh.qpX512;}
        else if(PanelControls::editBand(fresh,bandField,desiredValue,sampleRate,planned)!=PanelControls::Result::Ok){finish(PanelControls::Completion::Failed);return;}phase=Write;}
      else {bool ok=readback.type==planned.type&&readback.bypass==planned.bypass&&near(readback.frequencyHz,planned.frequencyHz)&&near(readback.q,planned.q)&&near(readback.gainDb,planned.gainDb)&&readback.qpX512==planned.qpX512;recipe=readback;finish(ok?PanelControls::Completion::Confirmed:PanelControls::Completion::Failed);}return;
    }
    if(operation==Route){if(r.size!=8||r.bytes[0]!=(valueSelector>>8)||r.bytes[1]!=(valueSelector&255)){finish(PanelControls::Completion::Unknown);return;}
      route={r.bytes[0],r.bytes[1],r.bytes[2]!=0,r.bytes[3]!=0,wireFloat(r.bytes+4)};
      if(phase==Read){if(PanelControls::editRoute(route,routeField,desiredValue,plannedRoute)!=PanelControls::Result::Ok){finish(PanelControls::Completion::Failed);return;}phase=Write;}
      else finish(route.enabled==plannedRoute.enabled&&route.invert==plannedRoute.invert&&near(route.gainDb,plannedRoute.gainDb)?PanelControls::Completion::Confirmed:PanelControls::Completion::Failed);return;}
    if(r.size!=f.length){finish(PanelControls::Completion::Unknown);return;}
    const unsigned width=kind==ValueKind::Float||kind==ValueKind::Dword?4:kind==ValueKind::Word?2:1;
    if(readOffset+width>r.size){finish(PanelControls::Completion::Unknown);return;}
    const uint8_t *bytes=r.bytes+readOffset;
    if(kind==ValueKind::Float)value=wireFloat(bytes);
    else {uint32_t integer=0;for(unsigned i=0;i<width;++i)integer|=uint32_t(bytes[i])<<(8*i);value=readMask?((integer&readMask)?1.0f:0.0f):float(integer);}
    if(!PanelControls::finite(value)){finish(PanelControls::Completion::Unknown);return;}
    if(phase==Read)phase=Write;else finish(near(value,desiredValue)?PanelControls::Completion::Confirmed:PanelControls::Completion::Failed);
  }
 private:
  enum Phase:uint8_t{Idle,Read,Write,Verify};enum Operation:uint8_t{Scalar,Eq,Route,Action};
  Wire wire;void *context;Phase phase=Idle;Operation operation=Scalar;ValueKind kind=ValueKind::Byte;
  uint32_t generation=0;bool didWrite=false,invalid=false,cancelled=false,createXoIfFlat=false;float desiredValue=0;
  uint8_t getRequest=0,setRequest=0,eqId=0,band=0,field=0;uint16_t valueSelector=0,readValue=0;
  PanelControls::BandField bandField=PanelControls::BandField::Type;PanelControls::RouteField routeField=PanelControls::RouteField::Gain;
  PanelControls::BandRecipe planned{},readback{},copyRecipe{};PanelControls::Route plannedRoute{};bool copyBand=false;
  bool validScalar(uint8_t get,uint8_t set,uint16_t selector,float v,ValueKind k)const{
    if(!PanelControls::finite(v))return false;
    if(k==ValueKind::Byte&&(v<0||v>255||v!=floorf(v)))return false;
    if(k==ValueKind::Word&&(v<0||v>65535||v!=floorf(v)))return false;
    if(k==ValueKind::Dword&&(v<0||v>=4294967296.0f||v!=floorf(v)))return false;
    if(get==0xee&&set==0xed)return selector==0&&k==ValueKind::Dword&&(v==44100||v==48000||v==96000);
    if(get==0x75&&set==0x74)return map.writable&&selector<map.outputs&&k==ValueKind::Float&&v>=-60&&v<=0;
    if(get==0x79&&set==0x78)return k==ValueKind::Float&&v>=0&&v<=maxDelayMs(map,uint8_t(selector),sampleRate)&&selector<map.outputs;
    if((get==0x77&&set==0x76)||(get==0x73&&set==0x72))return map.writable&&selector<map.outputs&&k==ValueKind::Byte&&(v==0||v==1);
    if((get==0x89&&set==0x88)||(get==0x99&&set==0x98)||(get==0x69&&set==0x68)||(get==0x6d&&set==0x6c)||
       (get==0x59&&set==0x58)||(get==0x5f&&set==0x5e)||(get==0xb5&&set==0xb4)||(get==0x31&&set==0x30)||(get==0x11&&set==0x10))
      return selector==0&&k==ValueKind::Byte&&(v==0||v==1);
    if(get==0x3f&&set==0x3e)return selector==0&&k==ValueKind::Float&&(v==0||v==1);
    return false;
  }
  bool begin(Operation op,float v){if(busy()||!PanelControls::finite(v))return false;operation=op;desiredValue=v;phase=Read;field=0;didWrite=invalid=cancelled=createXoIfFlat=copyBand=false;nativeStatus=0;readBytes=readOffset=readMask=0;completion=PanelControls::Completion::Pending;return true;}
  void finish(PanelControls::Completion c){completion=c;phase=Idle;}
};
}
#endif
