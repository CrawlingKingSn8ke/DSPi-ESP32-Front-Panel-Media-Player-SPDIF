#define main priorRuntimeMain
#include "Test-PanelRuntime.cpp"
#undef main
static void noWritesSince(unsigned before){for(unsigned i=before;i<nativeWire.calls.size();++i)CHECK(nativeWire.calls[i].get);}
int main(){using namespace PanelRuntime;
 strcpy(dspi.presetNames[0],"Original");passes(10);enterPage(PAGE_OUTPUTS);passes(16);
 nativeWire.routes[0].gainDb=-3.25f;nativeWire.routes[1].gainDb=-9.75f;nativeWire.gains[0]=-6;
 enterPage(PAGE_ROUTING);select(2);passes(8);CHECK(menuPage==PAGE_SUM&&panelMenuCount(menuPage)==3);
 CHECK(panelMenuName(menuPage,0)=="Left"&&panelMenuName(menuPage,1)=="Right"&&panelMenuName(menuPage,2)=="Mono L+R");
 CHECK(panelMenuValue(menuPage,0)=="-03.25"&&panelMenuValue(menuPage,1)=="-09.75"&&panelMenuValue(menuPage,2)=="Save");
 unsigned before=nativeWire.calls.size();select(0);CHECK(menuPage==PAGE_PANEL_NUMBER);panelNameMove(1);panelNameMove(1);panelAdjust(1);panelApply();select(2);
 CHECK(menuPage==PAGE_SUM&&panelMenuValue(menuPage,0)=="-03.24");
 select(1);panelNameMove(1);panelNameMove(1);panelAdjust(-1);panelApply();select(2);CHECK(panelMenuValue(menuPage,1)=="-09.76");noWritesSince(before);
 select(2);CHECK(menuPage==PAGE_PANEL_CONFIRM&&menuIndex==2);select(2);CHECK(menuPage==PAGE_SUM&&panelMenuValue(menuPage,0)=="-03.24");noWritesSince(before);
 select(0);panelAdjust(1);CHECK(panelBack()&&menuPage==PAGE_SUM&&panelMenuValue(menuPage,0)=="-03.24");select(2);CHECK(panelBack()&&menuPage==PAGE_SUM&&panelMenuValue(menuPage,1)=="-09.76");noWritesSince(before);
 select(2);select(3);passes(170);CHECK(fabsf(nativeWire.routes[0].gainDb+3.24f)<.0001f&&fabsf(nativeWire.routes[1].gainDb+9.76f)<.0001f);
 CHECK(nativeWire.routes[0].enabled&&nativeWire.routes[1].enabled&&nativeWire.routes[0].invert&&nativeWire.gains[0]==-6&&nativeWire.saveCount==0);
 for(unsigned i=2;i<8;++i)CHECK(!nativeWire.routes[i].enabled);
 // Stale selected routes must abort before any SET, even without a notification.
 enterPage(PAGE_SUM);passes(8);nativeWire.routes[1].gainDb=-4;select(2);before=nativeWire.calls.size();select(3);passes(35);noWritesSince(before);CHECK(!sumStage);
 // User-selected 0 dB is retained, with no hidden averaging attenuation.
 nativeWire.routes[0].gainDb=nativeWire.routes[1].gainDb=0;enterPage(PAGE_SUM);passes(8);select(2);select(3);passes(170);
 CHECK(nativeWire.routes[0].gainDb==0&&nativeWire.routes[1].gainDb==0&&nativeWire.gains[0]==-6);
 // An external change invalidates drafts; reopening is needed before save.
 enterPage(PAGE_SUM);passes(8);select(2);uint8_t changed[13]={2,2,0,1,0,0,1,0,1,0,0,0,1};panelNotification(changed,13);before=nativeWire.calls.size();select(3);passes(10);noWritesSince(before);CHECK(!sumStage);
 // Non-finite unrelated route is rejected by complete preflight before SET.
 enterPage(PAGE_SUM);passes(8);nativeWire.routes[7].gainDb=NAN;select(2);before=nativeWire.calls.size();select(3);passes(35);noWritesSince(before);CHECK(!sumStage);nativeWire.routes[7].gainDb=-4;
 nativeWire.routes[1].gainDb=NAN;enterPage(PAGE_SUM);passes(8);CHECK(panelMenuValue(menuPage,1)=="Unknown");before=nativeWire.calls.size();select(2);passes(5);noWritesSince(before);CHECK(menuPage==PAGE_SUM&&!sumStage);nativeWire.routes[1].gainDb=0;
 // Multi-channel source pair is selected BEFORE editing its Left/Right gains.
 model.activeInputs=4;enterPage(PAGE_ROUTING);select(4);CHECK(menuPage==PAGE_SOURCE_PAIR);select(1);CHECK(menuPage==PAGE_SUM);passes(8);
 CHECK(panelMenuValue(menuPage,0)=="-04.00"&&panelMenuValue(menuPage,1)=="-04.00");CHECK(panelBack()&&menuPage==PAGE_SOURCE_PAIR);
 select(1);passes(8);select(2);select(3);passes(170);CHECK(nativeWire.routes[2].enabled&&nativeWire.routes[3].enabled&&!nativeWire.routes[0].enabled&&!nativeWire.routes[1].enabled&&nativeWire.routes[2].gainDb==-4&&nativeWire.routes[3].gainDb==-4);
 // Cancel after the first confirmed disable: no unsent changes continue.
 sourcePair=1;enterPage(PAGE_SUM);passes(8);select(2);select(3);for(unsigned n=0;n<50&&!(sumStage==3&&groupAt==1&&!control.busy());++n)pass();CHECK(sumStage==3&&groupAt==1&&!control.busy());panelBack();before=nativeWire.calls.size();passes(20);noWritesSince(before);CHECK(!sumStage);
 // Output gain and EQ gain use the same two-decimal display, without changing
 // delay/frequency precision or the master-volume step.
 model.output=0;enterPage(PAGE_OUTPUT);passes(10);CHECK(panelMenuValue(menuPage,1)=="-06.00");select(1);CHECK(panelMenuValue(menuPage,0)=="-06.00");panelNameMove(1);panelNameMove(1);panelAdjust(1);panelApply();select(2);passes(10);CHECK(fabsf(nativeWire.gains[0]+5.99f)<.0001f);
 puts("Editable summing: exact draft gains, explicit save, stale guard and two-decimal controls PASS");
}
