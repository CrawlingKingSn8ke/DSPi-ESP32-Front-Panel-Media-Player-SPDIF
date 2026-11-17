#pragma once
struct HostMdns {
  bool begin(const char*){return true;}
  void end(){}
  void setInstanceName(const char*){}
  template<class... T> void addService(T...){}
  template<class... T> void addServiceTxt(T...){}
};inline HostMdns MDNS;
inline int mdns_service_instance_name_set(const char*,const char*,const char*){return 0;}
inline int mdns_service_txt_item_remove(const char*,const char*,const char*){return 0;}
