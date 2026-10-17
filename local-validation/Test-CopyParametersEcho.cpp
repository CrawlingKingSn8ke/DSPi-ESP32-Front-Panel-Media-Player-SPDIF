#define main oldRuntimeMain
#include "Test-PanelRuntime.cpp"
#undef main
int main(){using namespace PanelRuntime;
 strcpy(dspi.presetNames[0],"Original");passes(10);enterPage(PAGE_OUTPUTS);passes(16);
 for(unsigned i=0;i<10;++i){nativeWire.eq[8][i]={1, uint8_t(i%2),float(100+i*100),.9f,float(i)-5,0};nativeWire.eq[9][i]={0,0,1000,.707f,0,0};}
 for(unsigned i=20;i<24;++i){nativeWire.eq[8][i]={uint8_t(32+i-20),0,float(80+(i-20)*100),.707f,0,0};nativeWire.eq[9][i]={0,0,1000,.707f,0,0};}
 nativeWire.eq[8][9]={11,0,120,.8f,80,512};
 nativeWire.gains[0]=-7.1f;nativeWire.delays[0]=1.25;nativeWire.mutes[0]=0;nativeWire.deferredEqEcho=true;
 model.output=0;enterPage(PAGE_OUTPUT);select(7);select(1);select(5);select(3);passes(400);
 CHECK(lastToast=="Parameters copied");
 for(unsigned i=0;i<24;++i)if(i<10||i>=20){CHECK(nativeWire.eq[9][i].type==nativeWire.eq[8][i].type);CHECK(nativeWire.eq[9][i].frequencyHz==nativeWire.eq[8][i].frequencyHz);CHECK(nativeWire.eq[9][i].gainDb==nativeWire.eq[8][i].gainDb);CHECK(nativeWire.eq[9][i].bypass==nativeWire.eq[8][i].bypass);CHECK(nativeWire.eq[9][i].q==nativeWire.eq[8][i].q);}
 CHECK(nativeWire.eq[9][9].qpX512==512);
 CHECK(nativeWire.gains[1]==-7.1f&&nativeWire.delays[1]==1.25f&&nativeWire.mutes[1]==0);
 auto start=[](){model.output=0;enterPage(PAGE_OUTPUT);select(7);select(1);select(5);select(3);};
 // Also exercise delivery outside the transaction, after service returned.
 nativeWire.queueDeferredEq=true;nativeWire.eq[8][0].frequencyHz=135.5f;nativeWire.eq[8][20].frequencyHz=95.5f;
 start();passes(400);CHECK(lastToast=="Parameters copied"&&nativeWire.eq[9][0].frequencyHz==135.5f&&nativeWire.eq[9][20].frequencyHz==95.5f);
 nativeWire.deferredEqSource=1;start();passes(150);CHECK(!copyJob.stage&&nativeWire.mutes[1]==1&&lastToast!="Parameters copied");
 nativeWire.deferredEqSource=0;nativeWire.alterDeferredEq=true;start();passes(150);CHECK(!copyJob.stage&&nativeWire.mutes[1]==1&&lastToast!="Parameters copied");
 nativeWire.deferredEqEcho=false;nativeWire.alterDeferredEq=false;
 // Match exact offset/data once, only while fresh. USB-origin or malformed
 // notifications never match, even when their data happens to be identical.
 clearEqEchoes();Frame f;f.get=false;f.request=0x42;f.length=16;f.bytes[0]=8;f.bytes[1]=0;f.bytes[2]=1;
 uint8_t e[28]={2,2,0,5,0x38,0x09,16}; // EQ channel8 band0: 824+8*12*16 =2360
 e[12]=1;rememberEqWrite(f);auto gen=generation;panelNotification(e,28);CHECK(generation==gen);panelNotification(e,28);CHECK(generation==gen+1);
 rememberEqWrite(f);gen=generation;nowMs+=2001;panelNotification(e,28);CHECK(generation==gen+1);
 rememberEqWrite(f);e[8]=1;gen=generation;panelNotification(e,28);CHECK(generation==gen+1);
 e[8]=0;rememberEqWrite(f);e[4]^=1;gen=generation;panelNotification(e,28);CHECK(generation==gen+1);
 e[4]^=1;rememberEqWrite(f);e[16]=1;gen=generation;panelNotification(e,28);CHECK(generation==gen+1);
 e[16]=0;rememberEqWrite(f);gen=generation;panelNotification(e,27);CHECK(generation==gen+1);
 puts("All 10 EQ bands and 4 crossovers survive deferred DSPi echoes: PASS");
}
