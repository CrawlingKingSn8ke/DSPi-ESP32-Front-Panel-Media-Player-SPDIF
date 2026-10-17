#include <stdio.h>
#include <stdlib.h>
#include "../firmware/DSPi_ESP32_Front_Panel_v1_1_2/PanelInputMenu.h"
#define CHECK(x)do{if(!(x)){printf("FAIL line %u: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(){
 PanelInputMenu m;CHECK(m.valid());CHECK(m.next(0,1,7)==1);
 CHECK(m.setVisible(2,false,7));CHECK(m.next(1,1,7)==0);CHECK(m.next(0,-1,7)==1);
 CHECK(m.setVisible(1,false,7));CHECK(!m.setVisible(0,false,7));CHECK(m.next(2,1,7)==0);
 CHECK(m.setVisible(2,true,7));CHECK(m.moveTo(2,1,7));CHECK(m.position(2,7)==1);CHECK(m.next(0,1,7)==2);
 CHECK(m.next(0,1,2)==1); // capability change leaves a bounded fallback, not an infinite loop
 CHECK(!m.moveTo(2,0,7));CHECK(!m.setVisible(7,true,7));m.order[1]=m.order[0];CHECK(!m.valid());
 puts("Input menu visibility, order, last-visible guard and fallback: PASS");
}
