#pragma once
#include "DspiLinkLifecycle.h"
#include "DspiLinkMenus.h"
#include "DspiLinkSetupMode.h"
bool linkOwnsPage(MenuPage);
String linkPageTitle(MenuPage);
uint8_t linkMenuCount(MenuPage);
String linkMenuName(MenuPage,uint8_t);
String linkMenuValue(MenuPage,uint8_t);
void linkSelect();void linkAdjust(int);void linkApply();
bool linkBack();bool linkPinMove(int);void linkCancelEdit();
void linkInit();void serviceLinkPanel();
bool linkAllowMusic();bool linkAllowTransfer();
void drawLinkPinCursor(uint8_t,int16_t,int16_t,const String&);
bool drawLinkDetails();
