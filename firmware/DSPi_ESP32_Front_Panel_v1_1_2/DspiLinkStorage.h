#pragma once
extern "C" {
#include "src/dspi_link_vendor/hub_auth.h"
#include "src/dspi_link_vendor/settings_model.h"
}
namespace DspiLink {
struct LinkNetworkSettings {bridge_settings_t bridge{};char ssid[33]{},password[65]{};bool enabled=false;};
struct SetupHotspot {char ssid[33]{},password[13]{};};
bool loadSetupHotspot(SetupHotspot&);
const board_table_t& panelLinkBoard();
bool loadLinkSettings(LinkNetworkSettings&);
bool saveLinkPreference(bool enabled);
bool saveLinkBridge(const bridge_settings_t&);
bool saveLinkNetwork(const char* ssid,const char* password);
bool forgetLinkNetwork();
bool loadLinkAuth(hub_auth_store_t&,bool& unreadable);
bool saveLinkAuth(const hub_auth_store_t&);
bool resetLinkStorage();
bool saveLinkDeviceName(const char* serial,const char* name);
bool loadLinkDeviceName(const char* serial,char* name,size_t size);
hub_auth_io_t linkCrypto();
}
