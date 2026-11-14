#pragma once
#include "PanelWifiPolicy.h"
namespace PanelWifi {
struct Snapshot {State state=State::Off;bool stationConnected=false;char ssid[33]{},ip[16]{};};
bool acquire(Owner owner);
bool owns(Owner owner);
bool release(Owner owner);
bool beginStation(Owner owner,const char* ssid,const char* password,const char* hostname);
bool beginCandidate(const char* ssid,const char* password);
void cancelCandidate();
bool connectedTo(const char* ssid);
void service(uint32_t nowMs);
Snapshot snapshot();
int signalStrength(); // Called only by the visible signal page, at 1Hz.
int powerSaveMode(); // Diagnostic sample, not a mode change.
}
