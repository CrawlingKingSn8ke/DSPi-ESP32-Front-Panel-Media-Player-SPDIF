#pragma once
#include "DspiLinkSockets.h"
namespace DspiLink {
enum class NetworkEventKind:uint8_t { Open,Text,Binary,Activity,Closed };
struct NetworkEvent {
  NetworkEventKind kind{};SocketToken token{};uint32_t address=0;
  uint8_t* data=nullptr;size_t length=0;
};
// Mutable asset snapshots are copied into a peer-owned response before sending.
struct NetworkAsset {const uint8_t* data=nullptr;size_t size=0;const char* type="text/plain";bool gzip=false;bool snapshot=false;};
using AssetLookup=NetworkAsset(*)(const char*);
using SetupHandler=unsigned(*)(uint32_t,const uint8_t*,size_t);
bool startNetwork(const char* hostname,const char* localIp,AssetLookup assets,SetupHandler setup=nullptr);
void stopNetwork();bool networkStopped();
bool nextNetworkEvent(NetworkEvent&);
void finishNetworkEvent(NetworkEvent&);
void releaseNetworkSession(SocketToken);
bool sendNetwork(SocketToken,uint8_t opcode,const uint8_t*,size_t);
void closeNetwork(SocketToken,uint16_t code);
void setNetworkInfo(const char* json);
}
