#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../firmware/DSPi_ESP32_Front_Panel_v1_1_2/PanelNumberEditor.h"
#define CHECK(x) do{if(!(x)){printf("FAIL line %u: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(){
  PanelNumberEditor n;CHECK(n.begin(126.5f,10,19845,1));CHECK(n.value()==126.5f);
  n.move(-10);n.adjust(1);CHECK(n.value()>126.59f&&n.value()<126.61f);
  n.move(1);n.adjust(-1);CHECK(n.value()>125.59f&&n.value()<125.61f);
  CHECK(n.begin(-9.9f,-24,12,1));n.move(-10);n.adjust(-1);CHECK(n.value()==-10.f);
  CHECK(n.begin(.707106f,.1f,20,2));CHECK(n.value()==.707106f); // no-op retains native precision
  n.move(-10);n.adjust(1);CHECK(n.value()>.719f&&n.value()<.721f);
  for(int i=0;i<3000;++i)n.adjust(-1);CHECK(n.value()>=.1f);
  char text[16];n.move(100);n.format(text,sizeof(text));CHECK(n.cursor(text)<strlen(text));CHECK(text[n.cursor(text)]>='0'&&text[n.cursor(text)]<='9');
  CHECK(!n.begin(NAN,0,1,1));CHECK(!n.begin(30,.1f,20,2));
  puts("Numeric editor precision, bounds, cursor and no-op retention: PASS");
}
