#include "DspiLinkSetupMode.h"
#include "DspiLinkSetupRequest.h"
#include "DspiLinkNetwork.h"
#include "DspiLinkHub.h"
#include "PanelWifi.h"
#include "SmbSetup.h"
#include "SmbSetupPage.h"
#include <esp_heap_caps.h>
#include <mutex>
#include <Arduino.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_wifi.h>
namespace DspiLink {
namespace {
struct SetupState {
  static void* operator new(size_t n,const std::nothrow_t&) noexcept{return heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);}
  static void operator delete(void* p) noexcept{free(p);}
  std::mutex policyMutex;
  PanelWifi::SetupPolicy policy;
  bool wifiOnly=false,finishRequested=false;
  SmbLibrary::Setup* shares=nullptr;char sharesJson[8192]{};
  char requestJson[SetupBodyLimit+1]{}; // policyMutex serializes this PSRAM scratch.
  char resultJson[128]{};
  hub_auth_t auth{};SetupWindow window;SetupStatus status{};uint32_t started=0,finishAt=0;
  bool saved=false,closing=false;
  // Scan and JSON are owned exclusively by the network worker until it stops.
  bool scanStarted=false,scanPending=false,scanTimedOut=false,scanStopping=false;
  uint32_t scanAt=0;char scanJson[8192]{};
};
SetupState* setup=nullptr;bool lastSaved=false;
// Static callback storage outlives every setup session. Arduino's user callback
// runs after its internal _scanDone() has finished touching the scan allocation.
bool scanFinished=false;
unsigned scanEvent=0;
const char page[]=R"HTML(<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>DSPi Link network setup</title>
<style>body{font:18px system-ui;background:#17191c;color:#eee;max-width:32em;margin:2em auto;padding:1em}label{display:block;margin:1em 0}input,select,button{box-sizing:border-box;width:100%;font:inherit;padding:.6em;margin-top:.3em;border-radius:.3em}button{background:#1686e8;color:white;border:0}small{color:#bbb}</style>
<h1>Connect DSPi to home Wi-Fi</h1><p>Choose your router's 2.4 GHz network below. Setup stays open for ten minutes. If your phone reports no internet, stay connected to the setup Wi-Fi.</p>
<label>Nearby Wi-Fi networks<select id="networks"><option value="">Searching…</option></select></label><button id="rescan" type="button">Scan again</button><p id="scanStatus" role="status"></p>
<form id="f"><label>Home Wi-Fi name<input id="s" maxlength="32" required autocomplete="off" autocapitalize="none" spellcheck="false"></label><small>Filled when you select a network. You can also enter a hidden network manually.</small><label>Home Wi-Fi password<input id="w" type="password" maxlength="63" autocomplete="off" autocapitalize="none" spellcheck="false"></label><label style="display:flex;align-items:center;gap:.5em"><input id="showPassword" type="checkbox" style="width:auto">Show Wi-Fi password</label><label>Link PIN<input id="p" type="password" inputmode="numeric" pattern="[0-9]{6}" maxlength="6" required autocomplete="off"></label><small>Use the six-digit Link PIN chosen on the ESP panel, not the hotspot or home Wi-Fi password. Forgotten it? Close setup on the panel, reopen Network Setup and choose Change PIN. There is no default PIN.</small><button>Save and connect</button></form><p id="r" role="status"></p><small>No internet connection is needed. Leave the Wi-Fi password empty only for an open home network.</small>
<script>
let saved=false,scanBusy=false;
showPassword.onchange=()=>{w.type=showPassword.checked?'text':'password'};
networks.onchange=()=>{if(networks.value)s.value=networks.value};
async function scanNetworks(restart=false){
 if(scanBusy||saved)return;scanBusy=true;rescan.disabled=true;scanStatus.textContent='Searching for 2.4 GHz networks…';
 try{
  let data;
  for(let attempt=0;attempt<20;attempt++){
   const response=await fetch(restart&&attempt===0?'/dspi/panel/rescan':'/dspi/panel/networks',{cache:'no-store'});
   if(!response.ok)throw Error('Scan unavailable');data=await response.json();
   if(data.state!=='scanning')break;
   await new Promise(resolve=>setTimeout(resolve,750));
  }
  if(saved)return;
  if(data.state!=='ready')throw Error('Scan unavailable');
  const selected=networks.value;networks.replaceChildren();
  const placeholder=document.createElement('option');placeholder.value='';placeholder.textContent='Select a network, or enter its name below';networks.appendChild(placeholder);
  for(const network of data.networks){const option=document.createElement('option');option.value=network.ssid;option.textContent=network.ssid+' ('+network.rssi+' dBm'+(network.open?', open':'')+')';networks.appendChild(option)}
  networks.value=selected;
  scanStatus.textContent=data.networks.length?'Select your network above. Hidden networks can be entered manually.':'No networks found. Scan again or enter the Wi-Fi name manually.';
 }catch(e){scanStatus.textContent='Could not scan. Try Scan again or enter the Wi-Fi name manually.'}
 finally{scanBusy=false;rescan.disabled=saved}
}
rescan.onclick=()=>scanNetworks(true);
const firstScan=scanNetworks();
f.onsubmit=async e=>{e.preventDefault();const b=f.querySelector('button');if(saved||b.disabled)return;b.disabled=true;r.textContent='Saving…';try{const a=await fetch('/dspi/panel/network',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:s.value,password:w.value,pin:p.value})});if(!a.ok)throw Error(a.status===429?'Too many PIN attempts. Try again later.':a.status===403?'PIN incorrect or setup window closed.':'Could not save. Check the network name and password.');r.textContent=await a.text();saved=true;rescan.disabled=true;b.textContent='Saved';f.reset()}catch(e){r.textContent=e.message}finally{b.disabled=saved}};
</script></html>)HTML";
NetworkAsset scanAsset(bool restart){
  if(!setup||__atomic_load_n(&setup->closing,__ATOMIC_ACQUIRE)||__atomic_load_n(&setup->saved,__ATOMIC_ACQUIRE)||!setup->window.active(millis()))return {};
  if(setup->scanPending&&__atomic_load_n(&scanFinished,__ATOMIC_ACQUIRE))setup->scanPending=false;
  int count=setup->scanPending?WIFI_SCAN_RUNNING:WiFi.scanComplete();
  if(!setup->scanStarted||(restart&&count!=WIFI_SCAN_RUNNING&&uint32_t(millis()-setup->scanAt)>=5000)){
    setup->scanStarted=true;setup->scanAt=millis();setup->scanTimedOut=false;setup->scanStopping=false;WiFi.scanDelete();
    __atomic_store_n(&scanFinished,false,__ATOMIC_RELEASE);
    count=WiFi.scanNetworks(true,false,false,120);
    setup->scanPending=count==WIFI_SCAN_RUNNING;
  }
  if(count==WIFI_SCAN_RUNNING&&uint32_t(millis()-setup->scanAt)>12000){
    if(!setup->scanStopping){esp_wifi_scan_stop();setup->scanStopping=true;}
    setup->scanTimedOut=true;
  }
  if(setup->scanTimedOut)count=WIFI_SCAN_FAILED;
  const char* state=count==WIFI_SCAN_RUNNING?"scanning":count<0?"failed":"ready";
  cJSON* json=cJSON_CreateObject();if(!json)return {};
  auto* rows=cJSON_AddArrayToObject(json,"networks");
  bool ok=rows&&cJSON_AddStringToObject(json,"state",state);
  // Bound result work and response size; never expose the setup SSID itself.
  for(int i=0;ok&&i<count&&i<255;++i){
    const String ssid=WiFi.SSID(i);if(!ssid.length()||ssid.length()>32||!strcmp(ssid.c_str(),setup->status.ssid)||!validUtf8((const uint8_t*)ssid.c_str(),ssid.length()))continue;
    const int rssi=WiFi.RSSI(i);int duplicate=-1;
    for(int j=0;j<cJSON_GetArraySize(rows);++j)if(!strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(rows,j),"ssid")->valuestring,ssid.c_str())){duplicate=j;break;}
    if(duplicate>=0){if(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(rows,duplicate),"rssi")->valueint>=rssi)continue;cJSON_DeleteItemFromArray(rows,duplicate);}
    int at=0;while(at<cJSON_GetArraySize(rows)&&cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(rows,at),"rssi")->valueint>=rssi)++at;
    if(at>=24)continue;
    auto* row=cJSON_CreateObject();
    if(!row||!cJSON_AddStringToObject(row,"ssid",ssid.c_str())||!cJSON_AddNumberToObject(row,"rssi",rssi)||!cJSON_AddBoolToObject(row,"open",WiFi.encryptionType(i)==WIFI_AUTH_OPEN)){cJSON_Delete(row);ok=false;break;}
    if(!cJSON_InsertItemInArray(rows,at,row)){cJSON_Delete(row);ok=false;break;}
    if(cJSON_GetArraySize(rows)>24)cJSON_DeleteItemFromArray(rows,24);
  }
  if(ok)ok=cJSON_PrintPreallocated(json,setup->scanJson,sizeof(setup->scanJson),false);
  cJSON_Delete(json);if(!ok)return {};
  return {(const uint8_t*)setup->scanJson,strlen(setup->scanJson),"application/json",false,true};
}
const char wifiPage[]=R"HTML(<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>DSPi WiFi setup</title>
<style>body{font:18px system-ui;background:#17191c;color:#eee;max-width:32em;margin:2em auto;padding:1em}label{display:block;margin:1em 0}input,select,button{box-sizing:border-box;width:100%;font:inherit;padding:.6em;margin-top:.3em}button{background:#1686e8;color:white;border:0}small{color:#bbb}</style>
<h1>Connect DSPi to Wi-Fi</h1><p>Select your 2.4 GHz home network. Stay connected to the setup Wi-Fi if your phone reports no internet.</p>
<label>Wi-Fi network<select id="networks"><option>Searching…</option></select></label><button id="scan" type="button">Scan again</button>
<form id="f"><label>Network name<input id="ssid" maxlength="32" required autocapitalize="none" spellcheck="false"></label><small>Select above, or type a hidden network name.</small><label>Password<input id="password" type="password" maxlength="63" autocomplete="off" autocapitalize="none"></label><label><input id="show" type="checkbox" style="width:auto"> Show password</label><button id="join">Connect</button></form><p id="result" role="status"></p>
<script>
const token=new URLSearchParams(location.hash.slice(1)).get('setup')||sessionStorage.getItem('dspiSetup')||'';
if(token.length===32)sessionStorage.setItem('dspiSetup',token);
history.replaceState(null,'',location.pathname);
let busy=false,scanning=false;
show.onchange=()=>password.type=show.checked?'text':'password';
networks.onchange=()=>{if(networks.value)ssid.value=networks.value};
async function refresh(restart=false){
 if(scanning||busy)return;scanning=true;scan.disabled=true;
 try{let data;for(let n=0;n<20;n++){let r=await fetch(restart&&n===0?'/dspi/panel/rescan':'/dspi/panel/networks',{cache:'no-store'});if(!r.ok)throw Error();data=await r.json();if(data.state!=='scanning')break;await new Promise(r=>setTimeout(r,750))}
 if(data.state!=='ready')throw Error();networks.replaceChildren();const first=document.createElement('option');first.value='';first.textContent='Select a network';networks.append(first);for(const network of data.networks){const option=document.createElement('option');option.value=network.ssid;option.textContent=network.ssid;networks.append(option)}}
 catch(e){result.textContent='Could not scan. Try again or enter the network name.'}finally{scanning=false;scan.disabled=false}
}
scan.onclick=()=>refresh(true);
async function finish(){try{await fetch('/dspi/panel/network',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({token,done:true})})}catch(e){}}
async function status(){
 try{const r=await fetch('/dspi/panel/setup-status',{cache:'no-store'});if(!r.ok)throw Error();const data=await r.json();
 if(data.state==='saved'){busy=false;join.disabled=true;result.textContent='Connected. Wi-Fi saved. Setup will close; reconnect your phone to home Wi-Fi.';sessionStorage.removeItem('dspiSetup');setTimeout(finish,1200);return}
 if(data.state==='failed'){busy=false;join.disabled=false;result.textContent='Could not connect or save. Check the password and try again. Your previous network has been kept.';return}
 if(data.state==='joining'){busy=true;join.disabled=true;result.textContent='Checking connection…';setTimeout(status,1000);return}
 if(data.state==='ready'&&busy){busy=false;join.disabled=false;result.textContent='Request was not accepted. Check the details and try again.'}
 }catch(e){if(busy){result.textContent='Connection is being checked. Rejoin the setup Wi-Fi if needed.';setTimeout(status,1500)}}
}
f.onsubmit=async e=>{e.preventDefault();if(busy)return;if(token.length!==32){result.textContent='Scan the setup page QR code shown on the panel.';return}busy=true;join.disabled=true;result.textContent='Checking connection…';
 try{const r=await fetch('/dspi/panel/network',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({token,ssid:ssid.value,password:password.value})});if(r.status!==202){busy=false;join.disabled=false;result.textContent='Request refused. Check the details, or reopen setup on the panel.';return}password.value='';setTimeout(status,750)}catch(e){result.textContent='Request may still be checking. Rejoin setup Wi-Fi if needed.';setTimeout(status,1500)}};
refresh();status();
</script></html>)HTML";
NetworkAsset asset(const char* path){
  if(setup&&setup->shares&&!strncmp(path,"/dspi/panel/smb/",16)){
    std::lock_guard<std::mutex> lock(setup->policyMutex);
    if(__atomic_load_n(&setup->closing,__ATOMIC_ACQUIRE)||!setup->policy.authorized(path+16,millis())||!setup->shares->list(setup->sharesJson,sizeof setup->sharesJson))return {};
    return {(const uint8_t*)setup->sharesJson,strlen(setup->sharesJson),"application/json",false,true};
  }
  if(setup&&setup->wifiOnly&&!strcmp(path,"/dspi/panel/setup-status")){
    std::lock_guard<std::mutex> lock(setup->policyMutex);
    const auto phase=setup->policy.phase();const char* state=phase==PanelWifi::SetupPhase::Joining||phase==PanelWifi::SetupPhase::SaveRequested?"joining":phase==PanelWifi::SetupPhase::Success?"saved":phase==PanelWifi::SetupPhase::Failed?"failed":"ready";
    snprintf(setup->resultJson,sizeof setup->resultJson,"{\"state\":\"%s\"}",state);
    return {(const uint8_t*)setup->resultJson,strlen(setup->resultJson),"application/json",false,true};
  }
  if(!strcmp(path,"/dspi/panel/networks"))return scanAsset(false);
  if(!strcmp(path,"/dspi/panel/rescan"))return scanAsset(true);
  if(!strcmp(path,"/")||!strcmp(path,"/index.html")){
    if(setup&&setup->shares)return {(const uint8_t*)SmbLibrary::setupPage,sizeof(SmbLibrary::setupPage)-1,"text/html; charset=utf-8",false};
    return setup&&setup->wifiOnly?NetworkAsset{(const uint8_t*)wifiPage,sizeof(wifiPage)-1,"text/html; charset=utf-8",false}:NetworkAsset{(const uint8_t*)page,sizeof(page)-1,"text/html; charset=utf-8",false};
  }
  return {};
}
unsigned saveNetwork(uint32_t address,const uint8_t* bytes,size_t size){
  if(setup&&setup->wifiOnly){
    if(__atomic_load_n(&setup->closing,__ATOMIC_ACQUIRE)||!setup->window.active(millis()))return 403;
    std::lock_guard<std::mutex> lock(setup->policyMutex);
    if(bytes&&size&&size<=SetupBodyLimit&&!memchr(bytes,0,size)&&validUtf8(bytes,size)){
      auto& text=setup->requestJson;memcpy(text,bytes,size);text[size]=0;
      if(!jsonDepthAllowed(text)||strstr(text,"\\u0000")){memset(text,0,sizeof text);return 400;}
      cJSON* json=cJSON_ParseWithLengthOpts(text,size+1,nullptr,true);
      if(json&&setup->shares&&cJSON_GetObjectItemCaseSensitive(json,"action")){
        const auto* token=cJSON_GetObjectItemCaseSensitive(json,"token");
        unsigned result=cJSON_IsString(token)&&setup->policy.authorized(token->valuestring,millis())?setup->shares->request(text):403;
        cJSON_Delete(json);memset(text,0,sizeof text);return result;
      }
      if(json&&cJSON_IsObject(json)&&cJSON_GetArraySize(json)==2){
        const auto* token=cJSON_GetObjectItemCaseSensitive(json,"token");const auto* done=cJSON_GetObjectItemCaseSensitive(json,"done");
        bool finish=cJSON_IsString(token)&&cJSON_IsTrue(done)&&setup->policy.authorized(token->valuestring,millis())&&setup->policy.phase()==PanelWifi::SetupPhase::Success&&(!setup->shares||!setup->shares->busy());
        cJSON_Delete(json);memset(text,0,sizeof text);
        if(finish){__atomic_store_n(&setup->finishRequested,true,__ATOMIC_RELEASE);return 200;}
        return 403;
      }
      if(json)cJSON_Delete(json);memset(text,0,sizeof text);
    }
    SetupCredentials credentials{};auto result=setupTokenCredentials(setup->policy,bytes,size,millis(),credentials);
    if(result==200){PanelWifi::Credentials candidate{};strcpy(candidate.ssid,credentials.ssid);strcpy(candidate.password,credentials.password);
      result=setup->policy.request(candidate,setup->status.token,millis())?202:409;memset(&candidate,0,sizeof candidate);}
    memset(&credentials,0,sizeof credentials);return result;
  }
  // Only the network worker touches auth/rate-limit state while this mode lives.
  if(!setup||__atomic_load_n(&setup->closing,__ATOMIC_ACQUIRE)||!setup->window.active(millis())||__atomic_load_n(&setup->saved,__ATOMIC_ACQUIRE))return 403;
  SetupCredentials credentials{};unsigned status=setupCredentials(setup->auth,address,bytes,size,credentials);
  if(status==200){
    if(saveLinkNetwork(credentials.ssid,credentials.password))__atomic_store_n(&setup->saved,true,__ATOMIC_RELEASE);
    else status=503;
  }
  memset(&credentials,0,sizeof(credentials));return status;
}
}
bool beginSetup(bool wifiOnly,bool shares=false){
  if(setup||!hubStopped()||!networkStopped())return false;
  hub_auth_store_t saved{};bool bad=false;if(!wifiOnly&&(!loadLinkAuth(saved,bad)||bad||!saved.pin_enabled))return false;
  setup=new(std::nothrow) SetupState();if(!setup)return false;
  setup->wifiOnly=wifiOnly;
  lastSaved=false;auto crypto=linkCrypto();hub_auth_init(&setup->auth,&crypto,&saved);
  SetupHotspot hotspot{};
  if(!loadSetupHotspot(hotspot)){delete setup;setup=nullptr;return false;}
  if(!scanEvent)scanEvent=WiFi.onEvent([](arduino_event_id_t){__atomic_store_n(&scanFinished,true,__ATOMIC_RELEASE);},ARDUINO_EVENT_WIFI_SCAN_DONE);
  if(!scanEvent){delete setup;setup=nullptr;return false;}
  if(!PanelWifi::acquire(PanelWifi::Owner::Setup)){delete setup;setup=nullptr;return false;}
  strcpy(setup->status.ssid,hotspot.ssid);strcpy(setup->status.password,hotspot.password);
  memset(&hotspot,0,sizeof(hotspot));
  WiFi.setAutoReconnect(false);WiFi.mode(WIFI_AP_STA);
  if(!WiFi.softAP(setup->status.ssid,setup->status.password,1,false,1)){
    PanelWifi::release(PanelWifi::Owner::Setup);delete setup;setup=nullptr;return false;
  }
  auto ip=WiFi.softAPIP().toString();snprintf(setup->status.ip,sizeof(setup->status.ip),"%s",ip.c_str());
  setup->started=millis();setup->window.open(setup->started);setup->status.active=true;
  if(wifiOnly){uint8_t entropy[16];esp_fill_random(entropy,sizeof entropy);setup->policy.open(entropy,setup->started);setup->policy.copyToken(setup->status.token);memset(entropy,0,sizeof entropy);}
  if(shares){
    void* memory=heap_caps_malloc(sizeof(SmbLibrary::Setup),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!memory){stopLinkSetup();return false;}
    setup->shares=new(memory) SmbLibrary::Setup;
    LinkNetworkSettings saved;loadLinkSettings(saved);
    if(saved.ssid[0]){PanelWifi::Credentials candidate{};strcpy(candidate.ssid,saved.ssid);strcpy(candidate.password,saved.password);setup->policy.request(candidate,setup->status.token,millis());memset(&candidate,0,sizeof candidate);}
    memset(saved.password,0,sizeof saved.password);
  }
  if(!startNetwork("dspi-setup",ip.c_str(),asset,saveNetwork)){stopLinkSetup();return false;}
  return true;
}
bool beginLinkSetup(){return beginSetup(false);}
bool beginWifiSetup(bool shares){return beginSetup(true,shares);}
void stopLinkSetup(){if(setup){__atomic_store_n(&setup->closing,true,__ATOMIC_RELEASE);setup->status.stopping=true;stopNetwork();}}
void serviceLinkSetup(){
  if(!setup)return;
  if(setup->shares){std::lock_guard<std::mutex> lock(setup->policyMutex);if(setup->status.stopping)setup->shares->cancel();else setup->shares->service(WiFi.status()==WL_CONNECTED);}
  if(setup->wifiOnly&&!setup->status.stopping){
    PanelWifi::Credentials candidate{};bool join=false,persist=false;
    {std::lock_guard<std::mutex> lock(setup->policyMutex);join=setup->policy.takeJoin(candidate);}
    if(join){PanelWifi::beginCandidate(candidate.ssid,candidate.password);memset(&candidate,0,sizeof candidate);}
    {std::lock_guard<std::mutex> lock(setup->policyMutex);
      setup->policy.service(millis(),PanelWifi::connectedTo(setup->policy.candidate().ssid));
      persist=setup->policy.phase()==PanelWifi::SetupPhase::SaveRequested;if(persist)candidate=setup->policy.candidate();}
    if(persist){const bool saved=saveLinkNetwork(candidate.ssid,candidate.password);memset(&candidate,0,sizeof candidate);
      std::lock_guard<std::mutex> lock(setup->policyMutex);setup->policy.persisted(saved);if(saved)__atomic_store_n(&setup->saved,true,__ATOMIC_RELEASE);}
  }
  if(__atomic_load_n(&setup->saved,__ATOMIC_ACQUIRE)&&(!setup->wifiOnly||__atomic_load_n(&setup->finishRequested,__ATOMIC_ACQUIRE))&&!setup->finishAt)setup->finishAt=millis()+500;
  if(!setup->window.active(millis())||(setup->finishAt&&int32_t(millis()-setup->finishAt)>=0))stopLinkSetup();
  if(setup->status.stopping){
    stopNetwork();if(!networkStopped())return;
    if(setup->shares){std::lock_guard<std::mutex> lock(setup->policyMutex);setup->shares->cancel();if(!setup->shares->stopped())return;setup->shares->~Setup();free(setup->shares);setup->shares=nullptr;}
    // Never release/restart Arduino's scan cache while its event callback runs.
    if(setup->scanPending&&!__atomic_load_n(&scanFinished,__ATOMIC_ACQUIRE)){
      if(!setup->scanStopping){esp_wifi_scan_stop();setup->scanStopping=true;}return;
    }
    lastSaved=__atomic_load_n(&setup->saved,__ATOMIC_ACQUIRE);esp_wifi_scan_stop();WiFi.scanDelete();
    if(!PanelWifi::release(PanelWifi::Owner::Setup))return;
    setup->policy.cancel();memset(&setup->auth,0,sizeof setup->auth);memset(&setup->status,0,sizeof setup->status);delete setup;setup=nullptr;
  }
}
SetupStatus linkSetupStatus(){
  if(!setup){SetupStatus s;s.saved=lastSaved;return s;}
  auto s=setup->status;s.saved=__atomic_load_n(&setup->saved,__ATOMIC_ACQUIRE);
  if(setup->wifiOnly){std::lock_guard<std::mutex> lock(setup->policyMutex);s.joining=setup->policy.phase()==PanelWifi::SetupPhase::Joining||setup->policy.phase()==PanelWifi::SetupPhase::SaveRequested;s.failed=setup->policy.phase()==PanelWifi::SetupPhase::Failed;}
  s.seconds=(setup->window.remaining(millis())+999)/1000;return s;
}
}
