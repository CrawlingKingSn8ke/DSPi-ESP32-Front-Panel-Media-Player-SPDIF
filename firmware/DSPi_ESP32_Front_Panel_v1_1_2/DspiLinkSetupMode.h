#pragma once
#include <stdint.h>
namespace DspiLink {
struct SetupStatus {bool active=false,stopping=false,saved=false,joining=false,failed=false;uint32_t seconds=0;char ssid[33]{},password[13]{},ip[16]{},token[33]{};};
bool beginLinkSetup();void serviceLinkSetup();void stopLinkSetup();
bool beginWifiSetup(bool shares=false);
SetupStatus linkSetupStatus();
}
