#define main oldPanelRuntimeMain
#include "Test-PanelRuntime.cpp"
#undef main
int main(){
 using namespace PanelRuntime;
 strcpy(dspi.presetNames[0],"Original");passes(10);enterPage(PAGE_OUTPUTS);passes(16);
 model.output=0;enterPage(PAGE_OUTPUT);passes(10);select(0);
 CHECK(panelMenuValue(PAGE_OUTPUT,0)=="Off");panelAdjust(1);CHECK(panelMenuValue(PAGE_OUTPUT,0)=="On");panelBack();
 enterPage(PAGE_OUTPUT);passes(8);select(1);CHECK(menuPage==PAGE_PANEL_NUMBER);
 panelNameMove(1);panelAdjust(1);panelApply();select(2);passes(10);CHECK(nativeWire.gains[0]>-11.91f&&nativeWire.gains[0]<-11.89f);
  model.roles.subs=4;enterPage(PAGE_SPEAKERS);passes(10);select(2);passes(8);CHECK(menuPage==PAGE_PANEL_NUMBER);panelNameMove(1);panelAdjust(1);panelApply();select(2);passes(30);CHECK(nativeWire.gains[2]>-11.91f&&nativeWire.gains[2]<-11.89f);
  model.source=2;enterPage(PAGE_INPUT_NAME);select(2);panelAdjust(-1);CHECK(panelMenuValue(menuPage,2)=="Off");panelApply();CHECK(panelNextInput(1,1)!=2);CHECK(panelPersistLocal());loadLocal();CHECK(!(inputMenu.visible&4));
  model.source=3;enterPage(PAGE_INPUT_NAME);select(3);panelAdjust(-1);panelAdjust(-1);panelApply();CHECK(inputMenu.position(3,availableSourceMask())==1);CHECK(panelNextInput(0,-1)==3);
  serialKnown=false;CHECK(panelNextInput(1,1)==2);serialKnown=true;
  inputMenuDirty=aliasesDirty=true;char failedKey[16];localKey("i",0,failedKey,sizeof(failedKey));preferences.failKey=failedKey;
  CHECK(!panelPersistLocal()&&inputMenuDirty&&!aliasesDirty);preferences.failKey.clear();CHECK(panelPersistLocal()&&!inputMenuDirty);
  strcpy(aliases.full[1],"Television");CHECK(panelSourceLabel(1,"S/PDIF",false)=="Television");
  model.output=0;enterPage(PAGE_OUTPUT);select(3);CHECK(panelMenuCount(menuPage)==2);CHECK(panelMenuName(menuPage,0)=="Bands");CHECK(!model.link);
  nativeWire.delays[0]=1.25;nativeWire.mutes[0]=1;nativeWire.eq[8][0].frequencyHz=123.5;nativeWire.eq[9][0].frequencyHz=456.5;for(unsigned i=20;i<24;++i){nativeWire.eq[8][i].type=0;nativeWire.eq[9][i].type=0;}
  // A single explicit copy replaces just native channel parameters, no routing/wiring.
  enterPage(PAGE_OUTPUT);select(7);CHECK(menuPage==PAGE_COPY_DESTINATION);select(1);CHECK(menuPage==PAGE_COPY_PREVIEW);select(5);CHECK(menuPage==PAGE_PANEL_CONFIRM&&menuIndex==2);
  unsigned before=nativeWire.calls.size();select(3);passes(20);for(unsigned i=before;i<nativeWire.calls.size();++i)CHECK(nativeWire.calls[i].get);
  passes(350);CHECK(nativeWire.eq[9][0].frequencyHz==123.5f&&nativeWire.gains[1]==nativeWire.gains[0]&&nativeWire.delays[1]==1.25f&&nativeWire.mutes[1]==1);CHECK(lastToast=="Parameters copied");
  for(unsigned i=before;i<nativeWire.calls.size();++i)CHECK(nativeWire.calls[i].req!=0x70&&nativeWire.calls[i].req!=0x72&&nativeWire.calls[i].req!=0x7c);
  auto startCopy=[](){model.output=0;enterPage(PAGE_OUTPUT);select(7);select(1);select(5);select(3);};
  // Cancelling or changing context during snapshot must produce no mutation.
  startCopy();passes(10);panelBack();before=nativeWire.calls.size();passes(20);CHECK(!copyJob.stage);
  for(unsigned i=before;i<nativeWire.calls.size();++i)CHECK(nativeWire.calls[i].get);
  startCopy();nativeWire.notifyDuringRead=true;before=nativeWire.calls.size();passes(20);CHECK(!copyJob.stage);
  for(unsigned i=before;i<nativeWire.calls.size();++i)CHECK(nativeWire.calls[i].get);
  // Invalid source parameters are rejected before any target SET.
  nativeWire.gains[0]=1;startCopy();before=nativeWire.calls.size();passes(110);CHECK(!copyJob.stage&&lastToast=="Source outside panel range");
  for(unsigned i=before;i<nativeWire.calls.size();++i)CHECK(nativeWire.calls[i].get);
  nativeWire.gains[0]=-12;nativeWire.mutes[1]=0;nativeWire.rejectEq=9;startCopy();passes(140);
  CHECK(!copyJob.stage&&nativeWire.mutes[1]==1&&lastToast!="Parameters copied");nativeWire.rejectEq=255;
  // Once guarded, Back must stop the remaining copy rather than restore/unmute.
  startCopy();for(unsigned i=0;i<110&&!(copyJob.stage==2&&copyJob.write==1);++i)pass();
  CHECK(copyJob.stage==2&&copyJob.write==1);panelBack();before=nativeWire.calls.size();passes(20);
  CHECK(!copyJob.stage&&nativeWire.mutes[1]==1);for(unsigned i=before;i<nativeWire.calls.size();++i)CHECK(nativeWire.calls[i].get);
 puts("Outputs stage3 controls: PASS");return 0;
}
