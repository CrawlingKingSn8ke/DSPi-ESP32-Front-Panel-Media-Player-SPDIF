#pragma once
#include "SmbClient.h"
namespace SmbLibrary {
constexpr unsigned kShares=4;constexpr size_t kSettingsBytes=2048;
struct Share {uint32_t id=0,revision=0;char label[33]={0},root[257]={0};Credentials connection;};
struct Settings {uint32_t sequence=0,nextId=1;Share shares[kShares];};
bool validShare(const Share&);
bool replaceShare(Settings&,unsigned slot,const Share&,bool keepPassword);
bool removeShare(Settings&,unsigned slot);
size_t encodeSettings(const Settings&,uint8_t*,size_t capacity);
bool decodeSettings(const uint8_t*,size_t,Settings&);
// Main-loop/provisioning only. Uses a separate NVS namespace; no Wi-Fi/Link keys.
bool loadSettings(Settings&);
bool saveSettings(Settings&);
}
