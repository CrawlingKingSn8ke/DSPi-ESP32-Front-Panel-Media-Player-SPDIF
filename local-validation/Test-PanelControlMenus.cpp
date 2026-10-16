#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../firmware/DSPi_ESP32_Front_Panel_v1_1_2/PanelControlMenus.h"
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
using namespace PanelControlMenus;
struct FakeWire {
  Frame frames[128]{}; unsigned count=0; bool reject=false, timeout=false,appliedLost=false;
  float gain=-12;uint32_t rate=48000; PanelControls::BandRecipe band{1,0,1000,0.7f,0,321};
  PanelControls::Route route{1,2,true,true,-3};
  static Reply run(void *p,const Frame &f) {
    FakeWire &w=*static_cast<FakeWire*>(p); w.frames[w.count++]=f;
    Reply r{}; r.status=w.timeout?WireStatus::Unknown:WireStatus::Ok;
    if(w.timeout)return r;
    if(f.request==0x43){r.size=4;unsigned field=f.value&7;
      if(field==0)r.bytes[0]=w.band.type; else if(field==4)r.bytes[0]=w.band.bypass;
      else if(field==5){r.bytes[0]=w.band.qpX512&255;r.bytes[1]=w.band.qpX512>>8;}
      else PanelControls::putFloatLE(r.bytes,field==1?w.band.frequencyHz:field==2?w.band.q:w.band.gainDb);
    } else if(f.request==0x42){ if(!w.reject){w.band.type=f.bytes[2];w.band.bypass=f.bytes[3];memcpy(&w.band.frequencyHz,f.bytes+4,4);memcpy(&w.band.q,f.bytes+8,4);memcpy(&w.band.gainDb,f.bytes+12,4);if(f.length==18)w.band.qpX512=f.bytes[16]|(f.bytes[17]<<8);} }
    else if(f.request==0x71){r.size=8;r.bytes[0]=w.route.input;r.bytes[1]=w.route.output;r.bytes[2]=w.route.enabled;r.bytes[3]=w.route.invert;PanelControls::putFloatLE(r.bytes+4,w.route.gainDb);}
    else if(f.request==0x70){if(!w.reject){w.route.enabled=f.bytes[2];w.route.invert=f.bytes[3];memcpy(&w.route.gainDb,f.bytes+4,4);}}
    else if(f.request==0x75){r.size=4;PanelControls::putFloatLE(r.bytes,w.gain);}
    else if(f.request==0x74){if(!w.reject)memcpy(&w.gain,f.bytes,4);}
    else if(f.request==0x7c){r.size=1;r.bytes[0]=w.reject?3:0;}
    else if(f.request==0xee){r.size=8;uint32_t current=44100;memcpy(r.bytes,&current,4);memcpy(r.bytes+4,&w.rate,4);}
    else if(f.request==0xed){memcpy(&w.rate,f.bytes,4);}
    else {r.size=f.length;memset(r.bytes,0,r.size);}
    if(w.appliedLost&&!f.get){w.appliedLost=false;r.status=WireStatus::Unknown;}
    return r;
  }
};
static void drain(Controller &c){ for(unsigned i=0;i<120&&c.busy();++i)c.service(); CHECK(!c.busy()); }
int main(){
  CHECK(mainCount()==6);CHECK(strcmp(mainLabel(3),"Speakers")==0);CHECK(strcmp(mainLabel(5),"System")==0);
  CHECK(featureSemantic(0)==Feature::Equaliser);CHECK(featureSemantic(7)==Feature::Limiter);
  CHECK(featureMask(Feature::Equaliser)==0);CHECK(featureMask(Feature::Loudness)==1);CHECK(featureMask(Feature::Limiter)==64);
  Model m; CHECK(m.count(Page::Speakers)==6);CHECK(m.count(Page::Outputs)==1);
  m.map=PanelControls::channelMap(PanelControls::Profile::Rp2350Current);
  m.activeInputs=2;CHECK(m.count(Page::EqInputs)==2);CHECK(m.count(Page::Routing)==3);uint8_t outputEq=0;CHECK(PanelControls::outputEqId(m.map,0,outputEq)&&outputEq==8);
  m.activeInputs=0;
  CHECK(m.count(Page::Outputs)==9);CHECK(m.parent(Page::Output)==Page::Outputs);
  CHECK(m.count(Page::Bands)==10);CHECK(m.count(Page::Crossover)==4);
  CHECK(m.nativeBand(3,true)==23);CHECK(m.nativeBand(9,false)==9);
  CHECK(m.roles.left==0&&m.roles.right==0&&m.roles.subs==0);
  CHECK(m.assignRole(0,2,true));CHECK(!m.assignRole(1,2,true));CHECK(m.roles.left==4&&m.roles.right==0);
  CHECK(m.assignRole(0,2,false));CHECK(m.assignRole(2,2,true));
  CHECK(m.count(Page::PresetTools)==5);CHECK(m.parent(Page::PresetTools)==Page::Preset);
  NameEditor n;CHECK(n.begin("Room",31));n.move(1);n.adjust(1);CHECK(strcmp(n.text,"Rpom")==0);n.cancel();CHECK(!n.active);
  CHECK(!n.begin("bad_name",31));CHECK(n.begin("",31));CHECK(!n.save());n.adjust(1);CHECK(n.save());
  CHECK(n.begin("ABCDEFGHIJKLMNOPQRSTUVWXYZabcde",31));n.move(30);char preview[32];uint16_t cursorOffset=0,cursorWidth=0;
  CHECK(n.preview(preview,sizeof(preview),80,[](const char *s,void*)->uint16_t{return uint16_t(strlen(s)*10);},nullptr,cursorOffset,cursorWidth));
  CHECK(strlen(preview)<=8&&preview[strlen(preview)-1]=='e'&&cursorOffset<80&&cursorWidth==10);
  float outputValue=99;CHECK(!safeAdjust(-80,1,0.5f,-60,0,outputValue));CHECK(outputValue==99);
  CHECK(!safeAdjust(18,-1,0.5f,-24,12,outputValue));CHECK(safeAdjust(-6,1,0.5f,-60,0,outputValue)&&outputValue==-5.5f);
  CHECK(fabsf(maxDelayMs(m.map,0,48000)-42.645833f)<0.0001f);CHECK(fabsf(maxDelayMs(m.map,8,48000)-39.979167f)<0.0001f);
  CHECK(fabsf(maxDelayMs(m.map,0,96000)-21.322917f)<0.0001f);CHECK(fabsf(maxDelayMs(m.map,8,44100)-43.51474f)<0.0001f);
  CHECK(maxDelayMs(m.map,0,0)==0);
  FakeWire w;Controller c(FakeWire::run,&w);c.map=m.map;c.sampleRate=48000;
  CHECK(!c.startScalar(0x75,0x74,2,100,ValueKind::Float));CHECK(w.count==0);
  CHECK(!c.startScalar(0x75,0x74,2,-6,ValueKind::Byte));CHECK(!c.startScalar(0x77,0x76,2,1.5f,ValueKind::Byte));
  CHECK(!c.startScalar(0x77,0x76,2,256,ValueKind::Byte));CHECK(!c.startScalar(0x79,0x78,2,-1,ValueKind::Float));
  CHECK(c.startScalar(0xee,0xed,0,96000,ValueKind::Dword));c.readBytes=8;c.readOffset=4;drain(c);CHECK(w.rate==96000&&c.completion==PanelControls::Completion::Confirmed);CHECK(w.frames[1].length==4&&w.frames[1].bytes[0]==0&&w.frames[1].bytes[1]==0x77&&w.frames[1].bytes[2]==1&&w.frames[1].bytes[3]==0);
  w.count=0;
  CHECK(c.startScalar(0x75,0x74,2,-6,ValueKind::Float));CHECK(w.count==0);c.service();CHECK(w.count==1);drain(c);
  CHECK(c.completion==PanelControls::Completion::Confirmed);CHECK(w.gain==-6);CHECK(w.frames[0].request==0x75&&w.frames[1].request==0x74&&w.frames[2].request==0x75);
  CHECK(!w.frames[1].get&&w.frames[1].value==2&&w.frames[1].length==4);
  w.reject=true;CHECK(c.startScalar(0x75,0x74,2,-9,ValueKind::Float));drain(c);CHECK(c.completion==PanelControls::Completion::Failed);w.reject=false;
  unsigned before=w.count;CHECK(c.startScalar(0x75,0x74,2,-9,ValueKind::Float));c.cancel();c.service();CHECK(w.count==before);
  CHECK(c.startScalar(0x75,0x74,2,-9,ValueKind::Float));c.service();c.service();c.cancel();CHECK(c.busy());drain(c);CHECK(c.completion==PanelControls::Completion::Confirmed);
  CHECK(c.startBand(8,0,PanelControls::BandField::Frequency,1500));c.service();w.band.q=4.5f;drain(c);CHECK(c.completion==PanelControls::Completion::Confirmed);CHECK(w.band.q==4.5f&&w.band.qpX512==321&&w.band.frequencyHz==1500);
  CHECK(w.frames[w.count-7].request==0x42);CHECK(w.frames[w.count-7].bytes[0]==8&&w.frames[w.count-7].bytes[1]==0);
  w.band.type=11;w.band.gainDb=80;w.band.frequencyHz=40;w.band.qpX512=222;
  CHECK(c.startBand(8,2,PanelControls::BandField::Q,1.2f));drain(c);CHECK(w.band.qpX512==222&&w.band.gainDb==80);CHECK(w.frames[w.count-7].length==18);
  CHECK(c.startRoute(1,2,PanelControls::RouteField::Gain,-10));drain(c);CHECK(w.route.enabled&&w.route.invert&&w.route.gainDb==-10);
  CHECK(c.startScalar(0x75,0x74,2,-7,ValueKind::Float));c.invalidate();drain(c);CHECK(c.completion==PanelControls::Completion::Failed);
  w.appliedLost=true;before=w.count;CHECK(c.startScalar(0x75,0x74,2,-8,ValueKind::Float));c.service();c.service();CHECK(c.sent()&&c.busy());c.cancel();CHECK(c.busy());c.service();CHECK(!c.busy());CHECK(c.completion==PanelControls::Completion::Confirmed);CHECK(w.count==before+3&&w.frames[w.count-1].get);
  w.timeout=true;before=w.count;CHECK(c.startAction(0x7c,0x7d,1,8,ValueKind::Byte));c.service();CHECK(w.count==before+1);CHECK(c.busy()&&c.sent());c.cancel();c.service();CHECK(!c.busy());CHECK(c.completion==PanelControls::Completion::Unknown);CHECK(w.count==before+2&&w.frames[w.count-1].get);w.timeout=false;
  w.reject=true;CHECK(c.startAction(0x7c,0x7d,1,8,ValueKind::Byte));drain(c);CHECK(c.completion==PanelControls::Completion::Failed);CHECK(w.frames[w.count-1].get);
  before=w.count;for(unsigned i=0;i<20;++i)c.service();CHECK(w.count==before);
  printf("PanelControlMenus: %u checks passed\n",checks);
}
