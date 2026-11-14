#pragma once
#include "DspiLinkBroker.h"
extern "C" {
#include "src/dspi_link_vendor/dspi_link.h"
}
class HardwareSerial;
namespace DspiLink {
struct TransportStatus {
  uint32_t baud=115200;
  dspi_identity_t identity{};
  dspi_link_counters_t counters{};
  bool present=false, running=false, busy=false;
  uint32_t droppedNotifications=0;
  uint32_t generation=0;
};
using NotificationHandler=void(*)(const uint8_t*,uint16_t);
using ReplyHandler=void(*)(uint32_t,const dspi_reply_t&);
bool beginTransport(HardwareSerial& serial);
bool transportRunning();
// Main-loop only. Ownership persists through worker shutdown until reaped.
bool transportOwnsUart();
bool finishTransportStop();
void serviceTransport(NotificationHandler notification,ReplyHandler reply);
LinkAdmission submit(const TransportRequest& request,LinkTicket& ticket);
void requestStop();
bool stopped();
TransportStatus transportStatus();
void localJob(uint32_t job);
void networkSession(uint16_t slot,uint32_t generation);
// Called only by panel/main-loop helpers. A busy network/bulk operation
// returns immediately, rather than holding UI/BLE for the bulk wire time.
LinkAdmission executeLocal(const dspi_request_t&,bool mayRetry,uint8_t* out,
                          uint16_t capacity,dspi_reply_t& reply);
}
