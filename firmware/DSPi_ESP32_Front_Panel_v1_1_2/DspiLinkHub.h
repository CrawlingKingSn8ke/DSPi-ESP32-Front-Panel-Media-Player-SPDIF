#pragma once
#include "DspiLinkStorage.h"
#include "DspiLinkTransport.h"
namespace DspiLink {
struct HubStatus {bool active=false,online=false,stopping=false,pinSet=false,dspi=false;uint8_t clients=0;char ssid[33]{},ip[16]{},hostname[64]{},error[64]{};};
bool startHub(const LinkNetworkSettings&);
void serviceHub(bool localJob=false);
void stopHub();bool hubStopped();HubStatus hubStatus();
bool linkPinSet();bool setLinkPin(const char*);
bool revokeLinkClient(uint16_t cid);
void hubNativeNotification(const uint8_t*,uint16_t);
void hubNativeReply(uint32_t,const dspi_reply_t&);
bool hubAllowsLocal(const dspi_request_t&);
bool remoteAnalyserActive(uint32_t nowMs);
bool takeHubMutation();
bool takeHubControlChange();
bool hubExternalWritePossible();
}
