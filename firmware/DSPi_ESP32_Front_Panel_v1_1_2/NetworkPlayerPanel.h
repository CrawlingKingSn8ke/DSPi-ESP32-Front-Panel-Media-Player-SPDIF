#pragma once
#include "NetworkMenuModel.h"
#include "NetworkMediaPresentation.h"
#include "WifiSignal.h"
// MenuPage is declared by the sketch before including this internal header.
bool networkOwnsPage(MenuPage);
String networkPageTitle(MenuPage);
uint8_t networkMenuCount(MenuPage);
String networkMenuName(MenuPage,uint8_t);
String networkMenuValue(MenuPage,uint8_t);
bool networkMenuPlaying(MenuPage,uint8_t);
bool networkSelect();bool networkBack();void networkCancelPending();
bool networkOwnsAudio();bool networkIsArmed();bool networkHasTrack();
bool networkUsesNowPlaying();
bool networkAllowExclusive(NetworkMenus::Exclusive);
bool networkAllowSdTrack(int,int,bool,bool,bool,bool);
bool networkSetupFinished(bool);
bool networkWantsShareSetup();
void networkBeforeInputChange();void serviceNetworkPanel();
bool networkStopForMediaGuard();bool networkMediaGuardQuiet();
bool networkTogglePlayback();bool networkSkip(int);bool networkSeekRelative(int,uint16_t);
bool networkMediaPresentation(NetworkMediaPresentation&);
bool networkDrawSignal();
bool networkMoveMenu(int);
void networkRestoreBrowser();
bool networkBrowserEntry(uint8_t,const char*&,bool&);
void networkBrowserContext(const char*&,uint32_t&,uint32_t&);
