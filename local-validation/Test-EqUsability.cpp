// Executes the real menu adapter; only the existing wire/display shims are fake.
#define main originalRuntimeTests
#include "Test-PanelRuntime.cpp"
#undef main
static void ready(){
  using namespace PanelRuntime;
  panelCancel();nativeWire=Native();control=PanelControlMenus::Controller(exchange,nullptr);
  model=PanelControlMenus::Model();model.map=PanelControls::channelMap(PanelControls::Profile::Rp2350Current);
  model.eqOutput=true;model.activeInputs=2;control.map=model.map;control.sampleRate=44100;
  dspi.sampleRate=44100;dspi.activePreset=0;dspi.connected=true;
  capabilityAttempted=identityAttempted=serialKnown=true;activeInputStep=1;roleSlot=0;
  strcpy(serial,"0123456789ABCDEF");generation=0;editActive=false;lastToast="";
}
int main(){
  ready();enterPage(PAGE_EQ_TARGET);
  CHECK(panelMenuCount(PAGE_EQ_TARGET)==3);
  CHECK(panelMenuName(PAGE_EQ_TARGET,2)=="Crossover");
  select(2);CHECK(menuPage==PAGE_CROSSOVER);CHECK(panelBack());CHECK(menuPage==PAGE_EQ_TARGET);
  PanelRuntime::model.eqOutput=false;CHECK(panelMenuCount(PAGE_EQ_TARGET)==2);
  ready();enterPage(PAGE_SPEAKERS);CHECK(panelMenuName(menuPage,4)=="Channels");select(4);CHECK(menuPage==PAGE_OUTPUTS);
  ready();PanelRuntime::model.target=1;enterPage(PAGE_EQ_TARGET);select(0);CHECK(PanelRuntime::model.link);
  // Existing numeric source 126.5 must allow the requested 0.1 Hz edit.
  ready();nativeWire.eq[8][0].frequencyHz=126.5f;enterPage(PAGE_EQ_BAND);passes(8);select(2);
  CHECK(menuPage!=PAGE_EQ_BAND); // dedicated numeric screen, not ambiguous inline editing
  unsigned before=nativeWire.calls.size();panelNameMove(1);panelAdjust(1);panelApply();select(2);passes(30);
  CHECK(nativeWire.eq[8][0].frequencyHz>126.59f&&nativeWire.eq[8][0].frequencyHz<126.61f);
  ready();enterPage(PAGE_EQ_BAND);passes(8);select(0);CHECK(panelEditUnderline(menuPage,0));panelBack();CHECK(!panelEditUnderline(menuPage,0));
  ready();nativeWire.eq[8][0].frequencyHz=126.5f;enterPage(PAGE_EQ_BAND);passes(8);select(2);panelAdjust(1);before=nativeWire.calls.size();panelBack();passes(10);CHECK(menuPage==PAGE_EQ_BAND);CHECK(nativeWire.eq[8][0].frequencyHz==126.5f);for(unsigned i=before;i<nativeWire.calls.size();++i)CHECK(nativeWire.calls[i].req!=0x42);
  // Blank partner must be confirmed BEFORE either native band is mutated.
  ready();PanelRuntime::model.link=true;nativeWire.eq[8][0].frequencyHz=126.5f;nativeWire.eq[9][0].type=0;
  enterPage(PAGE_EQ_BAND);passes(8);select(2);panelNameMove(1);panelAdjust(1);panelApply();before=nativeWire.calls.size();select(2);passes(10);
  CHECK(menuPage==PAGE_PANEL_CONFIRM&&menuIndex==2);for(unsigned i=before;i<nativeWire.calls.size();++i)CHECK(nativeWire.calls[i].req!=0x42);
  select(3);passes(60);CHECK(nativeWire.eq[9][0].type==1);CHECK(nativeWire.eq[9][0].frequencyHz>126.59f&&nativeWire.eq[9][0].frequencyHz<126.61f);CHECK(lastToast=="Applied both channels");
  ready();PanelRuntime::model.target=1;PanelRuntime::model.link=true;nativeWire.eq[8][0].q=3.5f;
  enterPage(PAGE_EQ_BAND);passes(8);select(2);panelNameMove(1);panelAdjust(1);panelApply();select(2);passes(60);
  CHECK(nativeWire.eq[8][0].frequencyHz==1000.1f&&nativeWire.eq[9][0].frequencyHz==1000.1f);CHECK(nativeWire.eq[8][0].q==3.5f);
  ready();PanelRuntime::model.link=true;nativeWire.eq[9][0].type=0;enterPage(PAGE_EQ_BAND);passes(8);select(2);panelAdjust(1);panelApply();select(2);passes(10);CHECK(menuPage==PAGE_PANEL_CONFIRM);before=nativeWire.calls.size();select(2);passes(25);for(unsigned i=before;i<nativeWire.calls.size();++i)CHECK(nativeWire.calls[i].req!=0x42);CHECK(nativeWire.eq[8][0].frequencyHz==1000);
  ready();PanelRuntime::model.link=true;nativeWire.rejectEq=9;enterPage(PAGE_EQ_BAND);passes(8);select(2);panelAdjust(1);panelApply();select(2);passes(60);CHECK(lastToast=="Partial - verify state");CHECK(nativeWire.eq[9][0].frequencyHz==1000);
  ready();PanelRuntime::model.link=true;nativeWire.eq[8][20].type=nativeWire.eq[9][20].type=0;
  enterPage(PAGE_XO_BAND);passes(8);select(0);panelAdjust(1);panelApply();passes(60);CHECK(nativeWire.eq[8][20].type==32&&nativeWire.eq[9][20].type==32);
  // Cancelling a queued link, or changing its context, must prevent BOTH writes.
  ready();PanelRuntime::model.link=true;enterPage(PAGE_EQ_BAND);passes(8);select(2);panelAdjust(1);panelApply();select(2);before=nativeWire.calls.size();panelBack();passes(30);for(unsigned i=before;i<nativeWire.calls.size();++i)CHECK(nativeWire.calls[i].req!=0x42);
  ready();PanelRuntime::model.link=true;nativeWire.eq[9][0].type=0;enterPage(PAGE_EQ_BAND);passes(8);select(2);panelAdjust(1);panelApply();select(2);passes(8);CHECK(menuPage==PAGE_PANEL_CONFIRM);panelInvalidate();before=nativeWire.calls.size();select(3);passes(30);for(unsigned i=before;i<nativeWire.calls.size();++i)CHECK(nativeWire.calls[i].req!=0x42);
  // Gain crosses -10 without changing font or resolution; Q preserves finer native precision on no-op.
  ready();nativeWire.eq[8][0].gainDb=-9.9f;enterPage(PAGE_EQ_BAND);passes(8);select(3);panelNameMove(1);panelAdjust(-1);panelApply();select(2);passes(30);CHECK(nativeWire.eq[8][0].gainDb==-10.f);
  ready();nativeWire.eq[8][0].q=.707106f;enterPage(PAGE_EQ_BAND);passes(8);select(4);panelApply();select(2);passes(30);CHECK(nativeWire.eq[8][0].q==.707106f);
  // The panel's own 0.10 Qp encodes to51/512; it must reopen and increase.
  ready();nativeWire.eq[8][0].type=11;nativeWire.eq[8][0].gainDb=1000;nativeWire.eq[8][0].qpX512=51;
  enterPage(PAGE_EQ_BAND);passes(8);select(5);CHECK(menuPage==PAGE_PANEL_NUMBER);CHECK(panelMenuValue(menuPage,0)=="00.10");
  panelApply();select(2);passes(30);CHECK(nativeWire.eq[8][0].qpX512==51&&lastToast=="Applied");
  select(5);CHECK(menuPage==PAGE_PANEL_NUMBER);
  panelNameMove(1);panelNameMove(1);panelAdjust(1);panelApply();select(2);passes(30);CHECK(nativeWire.eq[8][0].qpX512==56);
  puts("EQ usability navigation: PASS");return 0;
}
