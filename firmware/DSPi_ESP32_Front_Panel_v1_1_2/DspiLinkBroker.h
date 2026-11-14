#pragma once
#include <stdint.h>
#include <string.h>
extern "C" {
#include "src/dspi_link_vendor/dspi_frame.h"
#include "src/dspi_link_vendor/link_policy.h"
}

namespace DspiLink {
enum class LinkOwner : uint8_t { Panel, Network, Telemetry };
enum class LinkAdmission : uint8_t { Accepted, Busy, Offline, Invalid };
struct LinkTicket { uint32_t generation=0, id=0; };
struct TransportRequest {
  dspi_request_t request{};
  LinkOwner owner=LinkOwner::Panel;
  uint16_t session=0;
  uint32_t sessionGeneration=0, job=0;
  bool mayRetry=false;
  LinkTicket ticket{};
};

// The target adapter serialises access. No UART or callbacks run under its lock.
// A cancelled active job retains its payload until the wire has been quarantined.
class Broker {
 public:
  static constexpr unsigned Capacity=12, MaxPayload=8192;
  static bool safeReplay(bool get,uint8_t request) {
    return get && link_policy_class(request,true)==LINK_CLASS_READ;
  }
  LinkAdmission submit(const TransportRequest& input,LinkTicket& ticket) {
    const auto& r=input.request;
    if(r.length>MaxPayload || (!r.is_get && r.length && !r.payload) ||
       (!r.is_get && r.request!=0xa1 && r.length>64) ||
       (r.is_get && r.request!=0xa0 && r.length>132)) return LinkAdmission::Invalid;
    if(localJob_ && (input.owner!=LinkOwner::Panel || input.job!=localJob_)) return LinkAdmission::Busy;
    const bool bulk=!r.is_get && r.length>64;
    if(bulk && bulkUsed_) return LinkAdmission::Busy;
    for(auto& slot:slots_) if(!slot.used) {
      slot.used=true; slot.cancelled=false; slot.active=false; slot.bulk=bulk;
      slot.work=input; slot.work.mayRetry=input.mayRetry && safeReplay(r.is_get,r.request);
      if(++sequence_==0) { ++epoch_; ++sequence_; }
      slot.work.ticket={epoch_,sequence_}; ticket=slot.work.ticket;
      if(!r.is_get && r.length) {
        auto* dst=bulk?bulk_:slot.small;
        memcpy(dst,r.payload,r.length); slot.work.request.payload=dst;
      } else slot.work.request.payload=nullptr;
      if(bulk)bulkUsed_=true;
      return LinkAdmission::Accepted;
    }
    return LinkAdmission::Busy;
  }
  TransportRequest* take(bool panelOnly=false) {
    if(active_)return nullptr;
    Slot* next=nullptr;
    for(auto& slot:slots_) if(slot.used && !slot.cancelled && (!panelOnly||slot.work.owner==LinkOwner::Panel)) {
      if(!next || priority(slot.work.owner)<priority(next->work.owner) ||
         (priority(slot.work.owner)==priority(next->work.owner) &&
          int32_t(slot.work.ticket.id-next->work.ticket.id)<0)) next=&slot;
    }
    if(!next)return nullptr;
    next->active=true;active_=next;return &next->work;
  }
  void finish(LinkTicket ticket) {
    for(auto& slot:slots_) if(slot.used && equal(slot.work.ticket,ticket)) {
      if(active_==&slot)active_=nullptr;
      if(slot.bulk)bulkUsed_=false;
      slot.used=false;return;
    }
  }
  void cancelSession(uint16_t session,uint32_t generation) {
    for(auto& slot:slots_) if(slot.used && slot.work.owner==LinkOwner::Network &&
       slot.work.session==session && slot.work.sessionGeneration==generation) {
      if(slot.active)slot.cancelled=true;else finish(slot.work.ticket);
    }
  }
  bool cancelled(LinkTicket ticket)const {
    for(const auto& slot:slots_)if(slot.used&&equal(slot.work.ticket,ticket))return slot.cancelled;
    return true;
  }
  bool busy()const {for(const auto& slot:slots_)if(slot.used)return true;return false;}
  bool active()const {return active_!=nullptr;}
  void setLocalJob(uint32_t job){localJob_=job;}
 private:
  struct Slot {TransportRequest work{};uint8_t small[64]{};bool used=false,active=false,cancelled=false,bulk=false;};
  static bool equal(LinkTicket a,LinkTicket b){return a.id==b.id&&a.generation==b.generation;}
  static unsigned priority(LinkOwner owner){return owner==LinkOwner::Panel?0:owner==LinkOwner::Network?1:2;}
  Slot slots_[Capacity]{};
  uint8_t bulk_[MaxPayload]{};
  Slot* active_=nullptr;
  uint32_t localJob_=0, sequence_=0, epoch_=1;
  bool bulkUsed_=false;
};
}
