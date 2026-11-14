#include "AudioDiagnostics.h"
// Main-loop-only glue: reuse the established list renderer and theme roles.
namespace LinkPanel {
static DspiLink::Lifecycle mode;
static DspiLink::LinkNetworkSettings settings;
enum class Confirm:uint8_t {None,Start,Stop,Music,Transfer,Forget,Reset,Client,Setup};
static Confirm confirmation=Confirm::None;
static bool bootRequested=false,preparationStarted=false,persistOn=false;
static char pin[7]="000000";static uint8_t cursor=0;static uint32_t redrawAt=0;
static uint16_t selectedClient=0;
static char selectedName[AUTH_CLIENT_NAME_BYTES]{};
static bool setupRequested=false,setupPreparing=false,setupWasActive=false;
static bool setupAfterPin=false,transferAfterStop=false;
static bool restartSaveFailed=false;
static uint32_t restartSaveFailedAt=0;
static uint8_t setupScreen=0;
static bool addressDetails=false;
static uint32_t wireGeneration=0,droppedNotices=0;
static DspiLink::LinkPage page(MenuPage p){return p==PAGE_WIFI?DspiLink::LinkPage::Wifi:static_cast<DspiLink::LinkPage>(int(p)-int(PAGE_LINK));}
static String displayText(const char* text){
  String out;for(const uint8_t* p=(const uint8_t*)text;*p;){
    if(*p<128){char ch=char(*p++);out+=ch==' '||ch=='+'||findGlyph(FontMedium,ch)?ch:'-';}
    else{out+='-';++p;while((*p&0xc0)==0x80)++p;}
  }return out;
}
static String shortText(const char* text){return ellipsizeFontText(FontMedium,displayText(text),140);}
static const char* phase(){using P=DspiLink::LinkPhase;switch(mode.phase()){
 case P::Off:return "Off";case P::Preparing:return "Preparing";case P::Joining:return "Joining";case P::Online:return "Online";case P::Reconnecting:return "Retrying";case P::Stopping:return "Stopping";default:return "Error";}}
static void confirm(Confirm action){confirmation=action;enterPage(PAGE_LINK_CONFIRM);menuIndex=0;drawMenu();}
static unsigned clients(hub_auth_store_t& auth){bool bad=false;if(!DspiLink::loadLinkAuth(auth,bad))return 0;unsigned count=0;for(auto& c:auth.clients)if(c.used)++count;return count;}
static const hub_auth_client_t* client(hub_auth_store_t& auth,unsigned row){clients(auth);for(auto& c:auth.clients)if(c.used){if(!row)return &c;--row;}return nullptr;}
static bool mediaBusy(){return mediaPlayerPoc.playbackState()!=MediaPlaybackState::Stopped||mediaTrackTransitionActive()||mediaArtworkJobSubmitted||mediaRouteRestorePending;}
static void start(){
  if(!networkAllowExclusive(NetworkMenus::Exclusive::Link))return;
  if(setupRequested||DspiLink::linkSetupStatus().active){showToast("Close network setup first");return;}
  if(!DspiLink::linkPinSet()){showToast("Set a Link PIN first");enterPage(PAGE_LINK_PIN);return;}
  DspiLink::loadLinkSettings(settings);
  if(!settings.ssid[0]){showToast("Set your WiFi network first");enterPage(PAGE_LINK_NETWORK);return;}
  if(mode.requestOn(mediaBusy())==DspiLink::ModeDecision::ConfirmStopMusic){confirm(Confirm::Start);return;}
  preparationStarted=false;persistOn=true;drawMenu();
}
static void stop(){if(mode.blocksMusic())AudioDiagnostics::event(AudioDiagnostics::Event::LinkStopping,millis());mode.stop();DspiLink::stopHub();drawMenu();}
static void openSetup(){
  if(!networkAllowExclusive(NetworkMenus::Exclusive::Setup))return;
  if(setupRequested||DspiLink::linkSetupStatus().active){enterPage(PAGE_LINK_SETUP);return;}
  setupAfterPin=false;confirm(Confirm::Setup);
}
}
bool linkOwnsPage(MenuPage p){return p>=PAGE_LINK&&p<=PAGE_LINK_PAIRING;}
String linkPageTitle(MenuPage p){using namespace LinkPanel;
  switch(p){case PAGE_WIFI:return "WiFi";case PAGE_LINK_PAIRING:return setupAfterPin?"Setup Link PIN":"Pair Console";case PAGE_LINK:return "DSPi Link";case PAGE_LINK_NETWORK:return "WiFi Network";case PAGE_LINK_CONNECTION:return "Connection";case PAGE_LINK_SECURITY:return "Security";case PAGE_LINK_ADDRESS:return "Link Address";case PAGE_LINK_PIN:return setupAfterPin?"Choose Link PIN":"Change PIN";case PAGE_LINK_CLIENTS:return "Paired Clients";case PAGE_LINK_CLIENT:return "Paired Client";case PAGE_LINK_SETUP:return setupScreen==0?"Join setup WiFi":setupScreen==1?"Open setup page":"Setup details";case PAGE_LINK_NETWORK_NAME:return "WiFi Network";case PAGE_LINK_CLIENT_NAME:return "Client Name";case PAGE_LINK_CONFIRM:
    switch(confirmation){case Confirm::Start:return "Start Link?";case Confirm::Music:case Confirm::Stop:case Confirm::Transfer:return "Stop Link?";case Confirm::Reset:return "Reset Link?";case Confirm::Forget:return "Forget WiFi?";case Confirm::Client:return "Remove Client?";case Confirm::Setup:return "Open Setup?";default:return "Confirm";}
    default:return "DSPi Link";}
}
uint8_t linkMenuCount(MenuPage p){if(p==PAGE_WIFI)return WifiSignal::menuCount(PanelWifi::snapshot().stationConnected);if(p==PAGE_LINK_PAIRING&&LinkPanel::setupAfterPin)return 2;if(p==PAGE_LINK_CLIENTS){hub_auth_store_t auth{};return std::max<unsigned>(1,LinkPanel::clients(auth));}return DspiLink::menuCount(LinkPanel::page(p));}
String linkMenuName(MenuPage p,uint8_t row){
  if(p==PAGE_WIFI){const bool connected=PanelWifi::snapshot().stationConnected;if(WifiSignal::isSignal(row,connected))return "Signal";row=WifiSignal::baseRow(row,connected);}
  if(p==PAGE_LINK_PAIRING&&LinkPanel::setupAfterPin)return row==0?"Use saved PIN":"Change PIN";
  if(p==PAGE_LINK_CLIENTS){hub_auth_store_t auth{};const auto* c=LinkPanel::client(auth,row);return c?LinkPanel::displayText(c->name):String("No paired clients");}
  return DspiLink::menuLabel(LinkPanel::page(p),row);
}
String linkMenuValue(MenuPage p,uint8_t row){using namespace LinkPanel;auto status=DspiLink::hubStatus();
  switch(p){
    case PAGE_LINK_PAIRING:return setupAfterPin?(row==0?"Continue":"Edit"):"";
    case PAGE_WIFI:{const bool connected=PanelWifi::snapshot().stationConnected;if(WifiSignal::isSignal(row,connected))return "View";row=WifiSignal::baseRow(row,connected);if(row==0)return settings.ssid[0]?shortText(settings.ssid):"Not set";return row==1?String(phase()):row==3?String(""):String("Open");}
    case PAGE_LINK:if(row==0)return phase();if(row==1)return "PIN";return row==2?"View":"PIN";
    case PAGE_LINK_NETWORK:return row==0?(settings.ssid[0]?shortText(settings.ssid):"Not set"):row==1?"Open Setup":row==2?"Confirm":"";
    case PAGE_LINK_CONNECTION:if(row==0)return phase();if(row==1)return status.dspi?"Connected":"Waiting";return row==2?"View":String(status.clients);
    case PAGE_LINK_SECURITY:if(row==0)return DspiLink::linkPinSet()?"On":"Not set";return row==1?"Edit":row==2?"View":"Confirm";
    case PAGE_LINK_ADDRESS:if(row==0)return status.ip[0]?String(status.ip):String("Not connected");if(row==1)return "11915";return shortText(settings.bridge.hostname);
    case PAGE_LINK_PIN:return row==0?String(pin):String("Apply");
    case PAGE_LINK_CLIENT:return row==0?String("View"):String("Confirm");
    default:return "";
  }
}
void linkInit(){using namespace LinkPanel;DspiLink::loadLinkSettings(settings);bootRequested=settings.enabled;}
bool linkAllowMusic(){using namespace LinkPanel;if(setupRequested||DspiLink::linkSetupStatus().active){showToast("Close network setup first");return false;}if(!mode.blocksMusic()){
  if(DspiLink::hubStatus().active||DspiLink::transportRunning()||!DspiLink::hubStopped()){showToast("Link is still stopping");return false;}
  return true;
}if(confirmation!=Confirm::Music||menuPage!=PAGE_LINK_CONFIRM)confirm(Confirm::Music);return false;}
bool linkAllowTransfer(){using namespace LinkPanel;if(!networkAllowExclusive(NetworkMenus::Exclusive::Transfer))return false;if(setupRequested||DspiLink::linkSetupStatus().active){showToast("Close network setup first");return false;}if(mode.mayTransfer())return true;confirm(Confirm::Transfer);return false;}
void linkSelect(){using namespace LinkPanel;
  if(menuPage==PAGE_LINK_ADDRESS){addressDetails=!addressDetails;drawMenu();return;}
  if(menuPage==PAGE_WIFI){
    const bool connected=PanelWifi::snapshot().stationConnected;
    if(WifiSignal::isSignal(menuIndex,connected)){enterPage(PAGE_WIFI_SIGNAL);return;}
    switch(DspiLink::wifiAction(WifiSignal::baseRow(menuIndex,connected))){
      case DspiLink::WifiAction::Network:enterPage(PAGE_LINK_NETWORK);break;
      case DspiLink::WifiAction::Setup:openSetup();break;
      case DspiLink::WifiAction::Link:enterPage(PAGE_LINK);break;
      case DspiLink::WifiAction::Transfer:
        if(setupRequested||DspiLink::linkSetupStatus().active){showToast("Close network setup first");break;}
        if(linkAllowTransfer())beginWifiTransferConfirmation();
        break;
      case DspiLink::WifiAction::Connection:enterPage(PAGE_LINK_CONNECTION);break;
      case DspiLink::WifiAction::Back:enterPage(PAGE_SYSTEM);break;
      default:break;
    }return;
  }
  if(menuPage==PAGE_LINK_SETUP){setupScreen=(setupScreen+1)%3;drawMenu();return;}
  if(menuPage==PAGE_LINK_PAIRING){
    if(setupAfterPin&&menuIndex==0){setupAfterPin=false;confirm(Confirm::Setup);return;}
    cursor=0;strcpy(pin,"000000");enterPage(PAGE_LINK_PIN);return;
  }
  if(DspiLink::linkSetupStatus().active&&(menuPage==PAGE_LINK_PIN||menuPage==PAGE_LINK_CONFIRM)){
    showToast("Close network setup first");return;
  }
  if(menuPage==PAGE_LINK_CONFIRM){
    const auto action=confirmation;confirmation=Confirm::None;
    if(menuIndex==0){transferAfterStop=false;if(action==Confirm::Setup&&networkSetupFinished(false))return;enterPage(action==Confirm::Setup||action==Confirm::Transfer?PAGE_WIFI:PAGE_LINK);return;}
    if(action==Confirm::Start){mode.confirmOn();preparationStarted=false;persistOn=true;enterPage(PAGE_LINK);}
    else if(action==Confirm::Stop||action==Confirm::Music||action==Confirm::Transfer){transferAfterStop=action==Confirm::Transfer;stop();enterPage(PAGE_LINK);}
    else if(action==Confirm::Forget){if(!networkAllowExclusive(NetworkMenus::Exclusive::Forget))return;stop();DspiLink::forgetLinkNetwork();settings.ssid[0]=0;settings.password[0]=0;enterPage(PAGE_LINK_NETWORK);}
    else if(action==Confirm::Reset){stop();DspiLink::resetLinkStorage();DspiLink::loadLinkSettings(settings);enterPage(PAGE_LINK);}
    else if(action==Confirm::Client){showToast(DspiLink::revokeLinkClient(selectedClient)?"Client removed":"Client removal failed");enterPage(PAGE_LINK_CLIENTS);}
    else if(action==Confirm::Setup){stop();setupScreen=0;setupRequested=true;setupPreparing=false;enterPage(PAGE_LINK_SETUP);}
    return;
  }
  if(menuPage==PAGE_LINK){if(menuIndex==0){if(mode.blocksMusic())confirm(Confirm::Stop);else start();}else enterPage(menuIndex==1?PAGE_LINK_PAIRING:menuIndex==2?PAGE_LINK_CONNECTION:PAGE_LINK_SECURITY);return;}
  if(menuPage==PAGE_LINK_NETWORK){if(menuIndex==3)enterPage(PAGE_WIFI);else if(menuIndex==2)confirm(Confirm::Forget);else if(menuIndex==1)openSetup();else enterPage(PAGE_LINK_NETWORK_NAME);return;}
  if(menuPage==PAGE_LINK_CONNECTION){if(menuIndex==2){addressDetails=false;enterPage(PAGE_LINK_ADDRESS);}else if(menuIndex==3)enterPage(PAGE_LINK_CLIENTS);return;}
  if(menuPage==PAGE_LINK_SECURITY){if(menuIndex<2){cursor=0;strcpy(pin,"000000");enterPage(PAGE_LINK_PIN);}else if(menuIndex==2)enterPage(PAGE_LINK_CLIENTS);else confirm(Confirm::Reset);return;}
  if(menuPage==PAGE_LINK_PIN){if(menuIndex==0){editActive=true;drawMenu();}else{if(DspiLink::setLinkPin(pin)){showToast("PIN saved");memset(pin,'0',6);if(setupAfterPin){setupAfterPin=false;confirm(Confirm::Setup);}else enterPage(PAGE_LINK_PAIRING);}else{showToast("PIN save failed");drawMenu();}}return;}
  if(menuPage==PAGE_LINK_CLIENTS){hub_auth_store_t auth{};const auto* c=client(auth,menuIndex);if(c){selectedClient=c->cid;snprintf(selectedName,sizeof(selectedName),"%s",c->name);enterPage(PAGE_LINK_CLIENT);}return;}
  if(menuPage==PAGE_LINK_CLIENT){if(menuIndex==1)confirm(Confirm::Client);else enterPage(PAGE_LINK_CLIENT_NAME);}
}
void linkAdjust(int direction){using namespace LinkPanel;if(menuPage==PAGE_LINK_PIN&&editActive){pin[cursor]=char('0'+(pin[cursor]-'0'+(direction>0?1:9))%10);drawMenu();}}
void linkApply(){using namespace LinkPanel;if(menuPage==PAGE_LINK_PIN&&editActive){if(cursor<5)++cursor;else{editActive=false;menuIndex=1;}drawMenu();}}
void linkCancelEdit(){
  editActive=false;LinkPanel::setupAfterPin=false;LinkPanel::transferAfterStop=false;
  if(menuPage==PAGE_LINK_CONFIRM)LinkPanel::confirmation=LinkPanel::Confirm::None;
  if(menuPage==PAGE_LINK_SETUP){LinkPanel::setupRequested=false;DspiLink::stopLinkSetup();}
}
bool linkPinMove(int direction){using namespace LinkPanel;if(menuPage!=PAGE_LINK_PIN||!editActive)return false;cursor=constrain(int(cursor)+direction,0,5);drawMenu();return true;}
bool linkBack(){
  if(!linkOwnsPage(menuPage))return false;
  if(editActive){editActive=false;drawMenu();return true;}
  if(menuPage==PAGE_LINK_CONFIRM){
    const auto action=LinkPanel::confirmation;linkCancelEdit();
    if(action==LinkPanel::Confirm::Setup&&networkSetupFinished(false))return true;
    enterPage(action==LinkPanel::Confirm::Transfer||action==LinkPanel::Confirm::Setup?PAGE_WIFI:PAGE_LINK);return true;
  }
  if(menuPage==PAGE_LINK_SETUP){LinkPanel::setupRequested=false;DspiLink::stopLinkSetup();if(!networkSetupFinished(false))enterPage(PAGE_WIFI);return true;}
  if((menuPage==PAGE_LINK_PIN||menuPage==PAGE_LINK_PAIRING)&&LinkPanel::setupAfterPin){LinkPanel::setupAfterPin=false;enterPage(PAGE_WIFI);return true;}
  const MenuPage parent=menuPage==PAGE_WIFI?PAGE_SYSTEM:menuPage==PAGE_LINK||menuPage==PAGE_LINK_NETWORK||menuPage==PAGE_LINK_CONNECTION?PAGE_WIFI:menuPage==PAGE_LINK_PIN||menuPage==PAGE_LINK_CLIENTS?PAGE_LINK_SECURITY:menuPage==PAGE_LINK_CLIENT?PAGE_LINK_CLIENTS:menuPage==PAGE_LINK_CLIENT_NAME?PAGE_LINK_CLIENT:menuPage==PAGE_LINK_NETWORK_NAME?PAGE_LINK_NETWORK:PAGE_LINK;
  enterPage(parent);return true;
}
void drawLinkPinCursor(uint8_t row,int16_t y,int16_t right,const String& value){using namespace LinkPanel;if(row||!editActive)return;
  const String before=String(pin).substring(0,cursor);const String digit=String(pin).substring(cursor,cursor+1);
  const FontDef &font=settingsListFont();
  int16_t x=right-fontTextWidth(font,value)+fontTextWidth(font,before);
  canvas->drawLine(x,y+33,x+fontTextWidth(font,digit)-1,y+33,uiAccent());
  canvas->drawLine(x,y+34,x+fontTextWidth(font,digit)-1,y+34,uiAccent());
}
namespace LinkPanel {
static void drawBrowserUrl(const char* url,int16_t y){
  if(retroTheme.enabled){drawFontCentred(FontSmall,y,url,uiAccent());return;}
  // The retained proportional font has no colon. Add it only in this URL
  // renderer; do not change any global glyph/font metrics or other screens.
  int16_t width=fontTextWidth(FontSmall,String(url));for(const char* p=url;*p;++p)if(*p==':')width+=5;
  int16_t x=(UI_W-width)/2;
  for(const char* p=url;*p;++p){
    if(*p==':'){canvas->fillRect(x+1,y+6,2,2,uiAccent());canvas->fillRect(x+1,y+11,2,2,uiAccent());x+=5;}
    else{char letter[]={*p,0};drawFontText(FontSmall,x,y,String(letter),uiAccent());x+=fontTextWidth(FontSmall,String(letter));}
  }
}
static int16_t detailWrapped(const char* text,int16_t y,uint16_t colour){
  String s=displayText(text);unsigned first=0;
  while(first<s.length()&&y<=215){
    unsigned end=first,next=first;
    while(next<s.length()){
      ++next;while(next<s.length()&&((uint8_t)s[next]&0xc0)==0x80)++next;
      if(fontTextWidth(FontMedium,s.substring(first,next))>UI_W-24)break;
      end=next;
    }
    if(end==first)end=next;
    drawFontCentredGlowColour(FontMedium,y,s.substring(first,end),colour);first=end;y+=28;
  }
  return y;
}
static bool drawPhoneQr(const char* text){
  static char previous[128]{};static uint8_t modules[37][37]{};static int size=0;
  if(strcmp(previous,text)){
    size=0;
    esp_qrcode_config_t config=ESP_QRCODE_CONFIG_DEFAULT();config.max_qrcode_version=5;config.qrcode_ecc_level=ESP_QRCODE_ECC_MED;
    config.display_func=[](esp_qrcode_handle_t qr){
      const int count=esp_qrcode_get_size(qr);if(count>37)return;size=count;
      for(int y=0;y<count;++y)for(int x=0;x<count;++x)modules[y][x]=esp_qrcode_get_module(qr,x,y);
    };
    if(esp_qrcode_generate(&config,text)!=ESP_OK||!size)return false;
    snprintf(previous,sizeof(previous),"%s",text);
  }
  if(!size)return false;
  const int scale=144/(size+8),side=(size+8)*scale,left=(UI_W-side)/2,top=50+(144-side)/2;
  canvas->fillRect(left,top,side,side,0xffff); // QR quiet zone must remain white, independent of theme.
  for(int y=0;y<size;++y)for(int x=0;x<size;++x)if(modules[y][x])canvas->fillRect(left+(x+4)*scale,top+(y+4)*scale,scale,scale,0);
  return true;
}
}
bool drawLinkDetails(){using namespace LinkPanel;
  if(menuPage==PAGE_LINK_PAIRING&&setupAfterPin)return false; // normal themed list: use saved PIN / change PIN
  if(menuPage!=PAGE_LINK_ADDRESS&&menuPage!=PAGE_LINK_SETUP&&menuPage!=PAGE_LINK_NETWORK_NAME&&menuPage!=PAGE_LINK_CLIENT_NAME&&menuPage!=PAGE_LINK_PAIRING)return false;
  uiView=VIEW_MENU;drawBase();drawFontCentredGlow(FontMedium,2,linkPageTitle(menuPage));
  drawTaperLine(UI_W/2,44,286,1,uiAccentDark());
  if(menuPage==PAGE_LINK_SETUP){
    auto s=DspiLink::linkSetupStatus();
    if(!s.active){drawFontCentredGlowColour(FontMedium,88,"Preparing setup",uiMainText());}
    else{
      char payload[128]{};
      if(setupScreen==0)snprintf(payload,sizeof(payload),"WIFI:T:WPA;S:%s;P:%s;;",s.ssid,s.password);
      else if(setupScreen==1)snprintf(payload,sizeof(payload),"http://%s:11915/#setup=%s",s.ip,s.token);
      if(setupScreen<2&&drawPhoneQr(payload)){
        char footer[32];snprintf(footer,sizeof(footer),"%02um %02us  OK next",unsigned(s.seconds/60),unsigned(s.seconds%60));
        drawFontCentredGlowColour(FontMedium,206,footer,uiMainText());flushCanvasLocked();return true;
      }
      drawFontCentredGlowColour(FontMedium,50,"Network",uiMainText());
      drawFontCentredGlowColour(FontMedium,78,s.ssid,uiAccent());
      drawFontCentredGlowColour(FontMedium,106,"Password",uiMainText());
      drawFontCentredGlowColour(FontMedium,134,s.password,uiAccent());
      drawFontCentredGlowColour(FontMedium,162,s.ip,uiMainText());
      drawFontCentredGlowColour(FontMedium,198,s.stopping?"Closing":"Port 11915",uiMainText());
    }
  }else if(menuPage==PAGE_LINK_PAIRING){
    drawFontCentredGlowColour(FontMedium,54,"Select DSPi in Console",uiMainText());
    drawFontCentredGlowColour(FontMedium,94,"Enter your Link PIN",uiAccent());
    drawFontCentredGlowColour(FontMedium,134,"The PIN you chose here",uiMainText());
    drawFontCentredGlowColour(FontMedium,198,"OK - change PIN",uiMainText());
  }else if(menuPage==PAGE_LINK_NETWORK_NAME){
    detailWrapped(settings.ssid[0]?settings.ssid:"Not set",64,uiAccent());
  }else if(menuPage==PAGE_LINK_CLIENT_NAME){
    detailWrapped(selectedName,64,uiAccent());
  }else{
    auto s=DspiLink::hubStatus();
    if(s.online&&s.ip[0]&&!addressDetails){
      char url[64];snprintf(url,sizeof url,"http://%s:11915/",s.ip);
      if(drawPhoneQr(url)){
        drawBrowserUrl(url,198);
        drawFontCentred(FontSmall,222,"OK - details",uiMainText());
        flushCanvasLocked();return true;
      }
    }
    drawFontCentredGlowColour(FontMedium,52,s.ip[0]?s.ip:"Not connected",uiAccent());
    drawFontCentredGlowColour(FontMedium,84,"Port 11915",uiMainText());
    drawFontCentredGlowColour(FontMedium,116,"Hostname",uiMainText());
    const String host=String(settings.bridge.hostname)+".local";
    detailWrapped(host.c_str(),148,uiAccent());
  }
  flushCanvasLocked();return true;
}
void serviceLinkPanel(){using namespace LinkPanel;
  DspiLink::serviceLinkSetup();
  auto setup=DspiLink::linkSetupStatus();
  if(setupWasActive&&!setup.active){
    setupRequested=false;DspiLink::loadLinkSettings(settings);showToast(setup.saved?"WiFi saved":"Setup closed");
    if(menuPage==PAGE_LINK_SETUP&&!networkSetupFinished(setup.saved))enterPage(PAGE_WIFI);
  }
  setupWasActive=setup.active;
  mode.transfer(wifiTransferExclusive());
  if(bootRequested){bootRequested=false;if(DspiLink::linkPinSet()&&settings.ssid[0]){mode.confirmOn();preparationStarted=false;}}
  const bool jobs=panelWorkPending()||presetSaveCompletionPending;
  DspiLink::localJob(jobs?1:0);DspiLink::serviceHub(jobs);
  if(DspiLink::takeHubControlChange()){
    externalRuntimeRefreshRequested=true;
    if(!notificationRefreshAt)notificationRefreshAt=millis()+DSPI_NOTIFICATION_REFRESH_MS;
  }
  if(DspiLink::transportRunning()){
    const auto wire=DspiLink::transportStatus();
    if(!wire.present)dspi.connected=false;
    if((wire.present&&wire.generation!=wireGeneration)||wire.droppedNotifications!=droppedNotices||DspiLink::takeHubMutation()){
      wireGeneration=wire.generation;droppedNotices=wire.droppedNotifications;
      panelInvalidate();stateRefreshRequested=true;externalRuntimeRefreshRequested=true;
      notificationRefreshAt=millis()+DSPI_NOTIFICATION_REFRESH_MS;
    }
  }
  using P=DspiLink::LinkPhase;
  if(setupRequested&&!setup.active&&DspiLink::hubStopped()&&mode.phase()==P::Off&&!wifiTransferExclusive()){
    if(!setupPreparing){stopMediaPlayback("Link network setup");setupPreparing=true;}
    const bool decoderQuiet=mediaPlayerPoc.serviceStopCleanup()&&mediaPlayerPoc.playbackState()==MediaPlaybackState::Stopped&&!mediaPlayerPoc.seeking()&&!mediaTrackTransitionActive();
    const bool artworkQuiet=!__atomic_load_n(&mediaArtworkWorkerBusy,__ATOMIC_ACQUIRE)&&!mediaArtworkRequestPending&&!mediaArtworkJobSubmitted;
    if(decoderQuiet&&artworkQuiet&&!mediaRoute.captured&&!mediaRouteRestorePending&&!jobs){
      setupRequested=false;if(!DspiLink::beginWifiSetup(networkWantsShareSetup())){networkSetupFinished(false);showToast("Setup could not open");enterPage(PAGE_LINK_NETWORK);}else{setupWasActive=true;drawMenu();}
    }
  }
  if(mode.phase()==P::Preparing){
    if(!preparationStarted){stopMediaPlayback("DSPi Link entry");preparationStarted=true;}
    const bool decoderQuiet=mediaPlayerPoc.serviceStopCleanup()&&mediaPlayerPoc.playbackState()==MediaPlaybackState::Stopped&&!mediaPlayerPoc.seeking()&&!mediaTrackTransitionActive();
    const bool artworkQuiet=!__atomic_load_n(&mediaArtworkWorkerBusy,__ATOMIC_ACQUIRE)&&!mediaArtworkRequestPending&&!mediaArtworkJobSubmitted;
    if(mode.prepared(decoderQuiet,artworkQuiet,!mediaRoute.captured&&!mediaRouteRestorePending,!jobs)){
      if(!DspiLink::beginTransport(DspiSerial)||!DspiLink::startHub(settings)){mode.fail();showToast("Link could not start");}
      else AudioDiagnostics::event(AudioDiagnostics::Event::LinkStarted,millis());
    }
  }
  auto status=DspiLink::hubStatus();
  static char loggedIp[16]{};
  if(status.online&&strcmp(loggedIp,status.ip)){
    snprintf(loggedIp,sizeof(loggedIp),"%s",status.ip);
    Serial.printf("DSPi Link: http://%s:11915/  http://%s.local:11915/\r\n",status.ip,settings.bridge.hostname);
  }else if(!status.online)loggedIp[0]=0;
  if(status.online){mode.connected();if(persistOn){DspiLink::saveLinkPreference(true);persistOn=false;}}
  else if(status.active&&!status.stopping)mode.connectionLost();
  if(mode.phase()==P::Stopping&&DspiLink::hubStopped()){
    DspiLink::requestStop();
    if(DspiLink::finishTransportStop()){
      // Network mode can leave internal heap fragmented for the decoder's
      // contiguous task stack. The agreed recovery is a clean panel restart,
      // never an early return to music with the post-network heap.
      if(jobs)return;
      if(restartSaveFailed&&uint32_t(millis()-restartSaveFailedAt)<1000)return;
      if(!DspiLink::saveLinkPreference(false)){
        if(!restartSaveFailed)showToast("Link save failed");
        restartSaveFailed=true;restartSaveFailedAt=millis();return;
      }
      persistOn=false;
      AudioDiagnostics::event(AudioDiagnostics::Event::LinkRestart,millis());
      AudioDiagnostics::preserveForRestart();
      showToast("Restarting panel",750);
      delay(750);
      ESP.restart();
      return; // Pending music/setup/transfer navigation is intentionally cancelled.
    }
  }
  if(transferAfterStop&&mode.phase()==P::Off&&DspiLink::hubStopped()){
    transferAfterStop=false;beginWifiTransferConfirmation();
  }
  if(linkOwnsPage(menuPage)&&!editActive&&uiView==VIEW_MENU&&uint32_t(millis()-redrawAt)>1000){redrawAt=millis();drawMenu();}
}
