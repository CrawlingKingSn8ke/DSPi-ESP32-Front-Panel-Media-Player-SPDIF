#pragma once
#include <stdint.h>
namespace DspiLink {
enum class LinkPage:uint8_t {Root,Network,Connection,Security,Address,Pin,Clients,Client,Confirm,Wifi};
enum class WifiAction:uint8_t {Network,Setup,Link,Transfer,Connection,Back,None};
inline WifiAction wifiAction(unsigned row){constexpr WifiAction actions[]={WifiAction::Network,WifiAction::Link,WifiAction::Transfer,WifiAction::Back};return row<4?actions[row]:WifiAction::None;}
inline uint8_t menuCount(LinkPage p){
  switch(p){case LinkPage::Root:case LinkPage::Connection:case LinkPage::Security:return 4;
    case LinkPage::Wifi:case LinkPage::Network:return 4;
    case LinkPage::Address:return 3;case LinkPage::Pin:case LinkPage::Client:case LinkPage::Confirm:return 2;default:return 1;}
}
inline const char* menuLabel(LinkPage p,unsigned row){
  static const char* root[]={"Link","Pair Console","Connection","Security"};
  static const char* wifi[]={"Network","DSPi Link","Transfer/Update","Back"};
  static const char* network[]={"WiFi","Configure","Forget Network","Back"};
  static const char* connection[]={"Network","DSPi","Address","Clients"};
  static const char* security[]={"PIN","Change PIN","Paired Clients","Reset Link"};
  static const char* address[]={"IP Address","Port","Hostname"};
  static const char* pin[]={"PIN","Save PIN"};
  static const char* client[]={"Name","Remove Client"};
  static const char* confirm[]={"Cancel","Continue"};
  if(row>=menuCount(p))return "";
  switch(p){case LinkPage::Wifi:return wifi[row];case LinkPage::Root:return root[row];case LinkPage::Network:return network[row];case LinkPage::Connection:return connection[row];case LinkPage::Security:return security[row];case LinkPage::Address:return address[row];case LinkPage::Pin:return pin[row];case LinkPage::Client:return client[row];case LinkPage::Confirm:return confirm[row];default:return "No paired clients";}
}
}
