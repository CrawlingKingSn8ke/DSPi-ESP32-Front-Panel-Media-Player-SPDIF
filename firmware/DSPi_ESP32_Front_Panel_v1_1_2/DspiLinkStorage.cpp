#include "DspiLinkStorage.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_random.h>
#include <esp_mac.h>
#include <mbedtls/sha256.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/md.h>
#include <time.h>
namespace DspiLink {
namespace {
constexpr char SettingsNamespace[]="dspi_link",AuthNamespace[]="dspi_lauth";
const uint8_t tx[]={17},rx[]={16};
const board_table_t board={"esp32s3",17,16,0,{tx,1},{rx,1},{nullptr,0}};
bool random(void*,uint8_t* out,size_t n){esp_fill_random(out,n);return true;}
bool hash(void*,const uint8_t* data,size_t n,uint8_t out[32]){return mbedtls_sha256(data,n,out,0)==0;}
bool pbkdf(void*,const char* pin,const uint8_t salt[16],uint8_t out[32]){
  return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,(const unsigned char*)pin,strlen(pin),salt,16,AUTH_PIN_PBKDF2_ITERATIONS,32,out)==0;
}
uint32_t now(void*){return millis();}
uint32_t unixTime(void*){auto seconds=time(nullptr);return seconds>=1704067200?uint32_t(seconds):0;}
void save(void*,const hub_auth_store_t* auth){if(!saveLinkAuth(*auth))Serial.println("DSPi Link: authentication storage failed");}
bool saveChecked(void*,const hub_auth_store_t* auth){return saveLinkAuth(*auth);}
bool fixed(const bridge_settings_t& b){return b.uart_tx_pin==17&&b.uart_rx_pin==16&&b.led_type==SETTINGS_LED_NONE&&b.led_pin==BOARD_PIN_NONE;}
}
const board_table_t& panelLinkBoard(){return board;}
hub_auth_io_t linkCrypto(){return {nullptr,random,hash,pbkdf,now,unixTime,save,saveChecked};}
bool loadSetupHotspot(SetupHotspot& out){
  out={};uint8_t mac[6]{};esp_read_mac(mac,ESP_MAC_WIFI_STA);
  char expected[33];snprintf(expected,sizeof(expected),"DSPi-Setup-%02X%02X%02X",mac[3],mac[4],mac[5]);
  Preferences p;if(!p.begin(SettingsNamespace,false))return false;
  SetupHotspot saved{};
  bool valid=p.getBytesLength("setup_ap")==sizeof(saved)&&p.getBytes("setup_ap",&saved,sizeof(saved))==sizeof(saved)&&
      strnlen(saved.ssid,sizeof(saved.ssid))<sizeof(saved.ssid)&&!strcmp(saved.ssid,expected)&&
      strnlen(saved.password,sizeof(saved.password))==12;
  if(valid)for(char c:saved.password)if(c&&!isalnum((unsigned char)c)){valid=false;break;}
  if(!valid){
    strcpy(saved.ssid,expected);uint8_t entropy[12];esp_fill_random(entropy,sizeof(entropy));
    static const char alphabet[]="ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    for(unsigned i=0;i<12;++i)saved.password[i]=alphabet[entropy[i]%(sizeof(alphabet)-1)];
    saved.password[12]=0;memset(entropy,0,sizeof(entropy));
    if(p.putBytes("setup_ap",&saved,sizeof(saved))!=sizeof(saved)){p.end();memset(&saved,0,sizeof(saved));return false;}
  }
  p.end();out=saved;memset(&saved,0,sizeof(saved));return true;
}
bool loadLinkSettings(LinkNetworkSettings& out){
  out={};uint8_t mac[6]{},nonce[16]{};esp_read_mac(mac,ESP_MAC_WIFI_STA);esp_fill_random(nonce,sizeof(nonce));
  settings_model_defaults(&out.bridge,&board,mac,nonce);out.bridge.uart_baud=115200;out.bridge.led_type=SETTINGS_LED_NONE;out.bridge.led_pin=BOARD_PIN_NONE;
  Preferences p;
  if(p.begin(SettingsNamespace,true)){
    out.enabled=p.getBool("enabled",false);
    bridge_settings_t saved{};
    if(p.getBytesLength("bridge")==sizeof(saved)&&p.getBytes("bridge",&saved,sizeof(saved))==sizeof(saved)){
      saved.hub_id[36]=0;saved.name[63]=0;saved.hostname[63]=0;
      if(fixed(saved)&&!settings_model_find_problem(&saved,&board))out.bridge=saved;
    }
    p.end();
  }
  // One canonical credential store, shared with the existing transfer page.
  if(p.begin("dspi_xfer",true)){
    String ssid=p.getString("sta_ssid",""),password=p.getString("sta_pass","");p.end();
    if(ssid.length()<=32&&password.length()<=63){ssid.toCharArray(out.ssid,sizeof(out.ssid));password.toCharArray(out.password,sizeof(out.password));}
  }
  return saveLinkBridge(out.bridge);
}
bool saveLinkPreference(bool enabled){Preferences p;if(!p.begin(SettingsNamespace,false))return false;bool ok=p.putBool("enabled",enabled)>0;p.end();return ok;}
bool saveLinkBridge(const bridge_settings_t& bridge){
  if(!fixed(bridge)||settings_model_find_problem(&bridge,&board))return false;
  Preferences p;if(!p.begin(SettingsNamespace,false))return false;
  bridge_settings_t old{};bool identical=p.getBytesLength("bridge")==sizeof(old)&&p.getBytes("bridge",&old,sizeof(old))==sizeof(old)&&!memcmp(&old,&bridge,sizeof(old));
  bool ok=identical||p.putBytes("bridge",&bridge,sizeof(bridge))==sizeof(bridge);p.end();return ok;
}
bool saveLinkNetwork(const char* ssid,const char* password){
  if(!ssid||!password||!strlen(ssid)||strlen(ssid)>32||strlen(password)>63||(strlen(password)&&strlen(password)<8))return false;
  Preferences p;if(!p.begin("dspi_xfer",false))return false;
  String oldSsid=p.getString("sta_ssid",""),oldPassword=p.getString("sta_pass","");
  p.putString("sta_ssid",ssid);p.putString("sta_pass",password);
  bool ok=p.isKey("sta_ssid")&&p.isKey("sta_pass")&&p.getString("sta_ssid","")==ssid&&p.getString("sta_pass","")==password;
  if(!ok){p.putString("sta_ssid",oldSsid);p.putString("sta_pass",oldPassword);}
  p.end();return ok;
}
bool forgetLinkNetwork(){Preferences p;if(!p.begin("dspi_xfer",false))return false;p.remove("sta_ssid");p.remove("sta_pass");p.end();return true;}
bool loadLinkAuth(hub_auth_store_t& out,bool& unreadable){
  out={};unreadable=false;Preferences p;
  if(!p.begin(AuthNamespace,true))return false;
  const size_t n=p.getBytesLength("store");
  if(n!=sizeof(out)){unreadable=n!=0;p.end();return false;}
  bool ok=p.getUChar("schema",0)==1&&p.getBytes("store",&out,sizeof(out))==sizeof(out);p.end();unreadable=!ok;return ok;
}
bool saveLinkAuth(const hub_auth_store_t& auth){
  Preferences p;if(!p.begin(AuthNamespace,false))return false;
  // Commit the blob last: a failed schema write must not replace the old PIN.
  bool ok=p.putUChar("schema",1)>0&&p.putBytes("store",&auth,sizeof(auth))==sizeof(auth);p.end();return ok;
}
bool resetLinkStorage(){
  // Deliberately no global erase, BLE namespace or audio-preset access.
  Preferences p;if(!p.begin(SettingsNamespace,false))return false;bool ok=p.clear();p.end();
  if(!p.begin(AuthNamespace,false))return false;ok=p.clear()&&ok;p.end();
  if(!p.begin("dspi_lnames",false))return false;ok=p.clear()&&ok;p.end();return ok;
}
namespace {
struct DeviceName {char serial[17]{},name[64]{};};
bool nameKey(const char* serial,char key[16]){
  if(!serial||!serial[0]||strnlen(serial,17)>16)return false;
  uint8_t digest[32];if(mbedtls_sha256((const uint8_t*)serial,strlen(serial),digest,0))return false;
  key[0]='n';for(unsigned i=0;i<7;++i)snprintf(key+1+2*i,3,"%02x",digest[i]);return true;
}
}
bool saveLinkDeviceName(const char* serial,const char* name){
  char key[16];if(!name||!nameKey(serial,key)||strnlen(name,64)>=64)return false;
  DeviceName entry;strcpy(entry.serial,serial);hub_auth_copy_name(entry.name,sizeof(entry.name),name);
  Preferences p;if(!p.begin("dspi_lnames",false))return false;
  bool ok=p.putBytes(key,&entry,sizeof(entry))==sizeof(entry);p.end();return ok;
}
bool loadLinkDeviceName(const char* serial,char* name,size_t size){
  char key[16];if(!name||!size||!nameKey(serial,key))return false;
  DeviceName entry;Preferences p;if(!p.begin("dspi_lnames",true))return false;
  bool ok=p.getBytesLength(key)==sizeof(entry)&&p.getBytes(key,&entry,sizeof(entry))==sizeof(entry);p.end();
  entry.serial[16]=0;entry.name[63]=0;if(!ok||strcmp(serial,entry.serial))return false;
  hub_auth_copy_name(name,size,entry.name);return true;
}
}
