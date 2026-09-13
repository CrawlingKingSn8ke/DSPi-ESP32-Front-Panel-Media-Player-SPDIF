#pragma once

#include <Arduino.h>

// Self-contained browser client for exclusive Wi-Fi transfer mode.
//
// HTTP API contract (all paths are relative to the transfer root):
//   GET  /api/status
//     -> {ok,mode,accepting,writerActive,currentFile,writtenBytes,
//         declaredBytes,freeBytes,totalBytes,message}
//   GET  /api/list?path=<dir>&page=<n>&limit=96
//     -> {ok,path,page,pageSize,hasMore,entries:[{name,path,type,size}]}
//   POST /api/mkdir
//     X-DSPi-Base: <percent-encoded current-dir, separators literal>
//     X-DSPi-Path: <percent-encoded new-relative-dir, separators literal>
//   POST /api/preflight
//     X-DSPi-Base: <percent-encoded current-dir, separators literal>
//     X-DSPi-Path: <percent-encoded picker-relative-path, separators literal>
//     X-DSPi-Declared-Size: <bytes>
//   PUT  /api/upload
//     Content-Type: application/octet-stream
//     X-DSPi-Base: <percent-encoded current-dir, separators literal>
//     X-DSPi-Path: <percent-encoded picker-relative-path, separators literal>
//     X-DSPi-Declared-Size: <bytes>
//     The request body is the unmodified File/Blob. The server must stream it
//     to final-name.ext.uploading and rename only after verified completion.
//   POST /api/cancel
//     Idempotently cancels/closes the current writer, leaving .uploading.
//   GET  /api/incomplete?path=<dir>&page=<n>&limit=96
//     -> {ok,entries:[{name,path,size}],page,hasMore}
//   DELETE /api/incomplete
//     X-DSPi-Path: <percent-encoded exact-.uploading-path, separators literal>
//     The server must reject every target that does not end in .uploading.
//   POST /api/delete
//     X-DSPi-Path: <percent-encoded exact existing file/folder path>
//     X-DSPi-Entry-Type: file | folder
//     X-DSPi-Confirm: DELETE
//     Permanently removes one listed music/JPEG file or a selected non-root
//     folder and its complete tree. The older DELETE /api/folder route remains
//     accepted for compatibility, but the shipped browser uses POST so captive
//     portal/proxy stacks cannot silently discard the operation.
//   GET  /api/network
//     -> {ok,saved,connected,connecting,ssid,lanIp,apIp,host}
//   GET  /api/network/scan
//     -> {ok,networks:[{ssid,rssi,secure}]}
//   POST /api/network
//     Form fields: ssid, password. Saves credentials in NVS and starts a
//     background STA connection while the direct AP remains available.
//   POST /api/finish
//     The server must reject this while a writer is active.
//   PUT  /api/firmware
//     Content-Type: application/octet-stream
//     X-DSPi-Declared-Size: <application-image bytes>
//     Streams one ESP32 application-only .bin to the inactive OTA slot.
//     Full/merged flash images and images larger than the OTA slot are rejected.
//
// Error responses use an HTTP error status and:
//   {ok:false,error:<stable-reason>,message:<safe-user-text>}
// All uint64 byte counts may be JSON numbers or decimal strings.
namespace WifiTransferWeb {

static constexpr uint16_t kDirectoryPageSize = 96U;

static constexpr char kRouteIndex[] = "/";
static constexpr char kRouteStatus[] = "/api/status";
static constexpr char kRouteList[] = "/api/list";
static constexpr char kRouteMkdir[] = "/api/mkdir";
static constexpr char kRoutePreflight[] = "/api/preflight";
static constexpr char kRouteUpload[] = "/api/upload";
static constexpr char kRouteCancel[] = "/api/cancel";
static constexpr char kRouteIncomplete[] = "/api/incomplete";
static constexpr char kRouteFolder[] = "/api/folder";
static constexpr char kRouteDelete[] = "/api/delete";
static constexpr char kRouteFinish[] = "/api/finish";
static constexpr char kRouteNetwork[] = "/api/network";
static constexpr char kRouteNetworkScan[] = "/api/network/scan";
static constexpr char kRouteFirmware[] = "/api/firmware";

static const char kIndexHtml[] PROGMEM = R"DSPITRANSFER(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="dark">
<title>DSPi Music Transfer</title>
<style>
:root{font-family:system-ui,-apple-system,"Segoe UI",sans-serif;color:#f7f9fc;background:#07111e;line-height:1.4;--panel:#101d2d;--line:#29405a;--accent:#5ab3ff;--bad:#ff8080;--good:#73d8a4}
*{box-sizing:border-box}
body{margin:0;min-height:100vh;background:linear-gradient(145deg,#07111e,#0c1a2a);color:#f7f9fc}
main{width:min(960px,100%);margin:auto;padding:18px clamp(12px,3vw,28px) 48px}
h1{margin:0;font-size:clamp(1.45rem,5vw,2.1rem);letter-spacing:.02em}
h2{font-size:1.05rem;margin:0 0 12px}
p{margin:.35rem 0}.sub,.muted{color:#b8c7d8}.sub{margin-bottom:18px}
.card{background:rgba(16,29,45,.96);border:1px solid var(--line);border-radius:14px;padding:14px;margin:12px 0;box-shadow:0 8px 24px #0004}
.statusgrid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:8px}
.stat{background:#0b1725;border-radius:9px;padding:9px;min-width:0}.stat span{display:block;color:#aebfd2;font-size:.78rem}.stat strong{display:block;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#notice{min-height:2.7rem;border-left:4px solid var(--accent)}#notice.bad{border-left-color:var(--bad)}#notice.good{border-left-color:var(--good)}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.row>*{min-width:0}
button,.button,input,select{font:inherit}
button,.button{appearance:none;border:1px solid #456582;background:#17314a;color:#fff;border-radius:9px;padding:9px 12px;min-height:42px;cursor:pointer;font-weight:650;text-align:center}
button:hover,.button:hover{background:#204462}button:focus-visible,.button:focus-visible,input:focus-visible,select:focus-visible{outline:3px solid #8dd0ff;outline-offset:2px}
button.primary{background:#0969a9;border-color:#238bd0}button.danger{background:#612d35;border-color:#9a505b}button.safe{background:#14633f;border-color:#24875a}
button:disabled,.button.disabled{opacity:.43;cursor:not-allowed}
input[type=text],input[type=password],select{flex:1;min-width:150px;background:#071321;color:#fff;border:1px solid #45617c;border-radius:9px;padding:10px;min-height:42px}
input[type=file]{position:absolute;left:-10000px;width:1px;height:1px}
.crumbs{display:flex;gap:5px;align-items:center;overflow-x:auto;padding:2px 0 10px}.crumbs button{white-space:nowrap;min-height:36px;padding:6px 9px}.sep{color:#7890a7}
.table{border:1px solid var(--line);border-radius:10px;overflow:hidden}.entry{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:10px;align-items:center;padding:10px;border-top:1px solid #21364d}.entry:first-child{border-top:0}.entry button.name{border:0;background:transparent;padding:0;text-align:left;min-height:32px;overflow:hidden;white-space:nowrap;display:flex;align-items:center;gap:9px}.entry .file{padding-left:8px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.size{color:#aebfd2;white-space:nowrap;font-variant-numeric:tabular-nums}
.folderIcon{position:relative;display:inline-block;width:19px;height:13px;border-radius:2px;background:#e7b84f;box-shadow:inset 0 -2px 0 #8e6824;flex:0 0 auto}.folderIcon:before{content:"";position:absolute;left:2px;top:-4px;width:9px;height:5px;border-radius:2px 2px 0 0;background:#f2cb70}.folderText{overflow:hidden;text-overflow:ellipsis}.legacyName{margin-left:7px;color:#90a7bd;font-size:.74rem;font-weight:500}.deleteEntry{white-space:nowrap;padding:7px 10px;min-height:36px}
.pager{justify-content:space-between;margin-top:10px}.queue{max-height:180px;overflow:auto;margin:8px 0;padding-left:24px}.queue li{padding:3px 0;overflow-wrap:anywhere}
progress{width:100%;height:14px;accent-color:var(--accent)}.meterlabel{display:flex;justify-content:space-between;gap:8px;font-variant-numeric:tabular-nums;font-size:.86rem;margin-top:7px}.meterlabel span:last-child{text-align:right}
.diagnostics{white-space:pre-wrap;overflow-wrap:anywhere;margin:0;font:500 .84rem/1.45 ui-monospace,SFMono-Regular,Consolas,monospace;color:#d8e6f4}.actions{position:sticky;bottom:8px;z-index:2;background:#101d2df2;backdrop-filter:blur(8px)}
.hidden{display:none!important}
@media(max-width:620px){.statusgrid{grid-template-columns:1fr 1fr}.stat:last-child{grid-column:1/-1}.entry{grid-template-columns:minmax(0,1fr) auto}.actions button{flex:1 1 42%}}
</style>
</head>
<body>
<main>
  <h1>DSPi Music Transfer</h1>
  <p class="sub">Copy music and JPEG cover art directly to the installed SD card. Keep the unit powered until Finish Safely completes.</p>

  <section class="card statusgrid" aria-label="Transfer status">
    <div class="stat"><span>Mode</span><strong id="mode">Connecting...</strong></div>
    <div class="stat"><span>Free space</span><strong id="free">--</strong></div>
    <div class="stat"><span>Writer</span><strong id="writer">Checking...</strong></div>
  </section>
  <section id="notice" class="card" role="status" aria-live="polite">Connecting to DSPi...</section>

  <section class="card">
    <h2>Home Wi-Fi access</h2>
    <p class="muted">Optional. Save your normal Wi-Fi details once, then use the LAN address shown below from any device on that network. The direct DSPi access point remains available.</p>
    <form id="networkForm">
      <div class="row">
        <button id="scanNetwork" type="button">Scan networks</button>
        <input id="wifiFilter" type="text" maxlength="32" autocomplete="off" placeholder="Filter scanned networks" aria-label="Filter scanned Wi-Fi networks">
        <select id="wifiNetworkSelect" aria-label="Scanned Wi-Fi networks">
          <option value="">Scan, then choose a network</option>
        </select>
      </div>
      <div class="row" style="margin-top:8px">
        <input id="wifiSsid" type="text" maxlength="32" autocomplete="ssid" placeholder="Wi-Fi network name (manual or hidden)" aria-label="Wi-Fi network name">
        <input id="wifiPassword" type="password" maxlength="63" autocomplete="current-password" placeholder="Wi-Fi password" aria-label="Wi-Fi password">
        <button id="saveNetwork" type="submit">Save and connect</button>
      </div>
    </form>
    <p id="networkStatus" class="muted">Checking saved network...</p>
  </section>

  <section class="card">
    <h2>Music Library</h2>
    <p id="storageNotice" class="muted">Checking SD card...</p>
    <nav id="crumbs" class="crumbs" aria-label="Current SD folder"></nav>
    <div class="row">
      <button id="parent" type="button">Up one folder</button>
      <button id="refresh" type="button">Refresh</button>
      <span id="folderLabel" class="muted"></span>
    </div>
    <div id="listing" class="table" aria-live="polite"></div>
    <p class="muted">Delete permanently removes a selected song, JPEG, or folder. Deleting a folder also removes everything inside it.</p>
    <div class="row pager">
      <button id="prevPage" type="button">Previous 96</button>
      <span id="pageLabel" class="muted"></span>
      <button id="nextPage" type="button">Next 96</button>
    </div>
  </section>

  <section class="card">
    <h2>Create folder</h2>
    <form id="mkdirForm" class="row">
      <input id="folderName" type="text" maxlength="255" autocomplete="off" placeholder="New folder name" aria-label="New folder name">
      <button type="submit">Create</button>
    </form>
  </section>

  <section class="card">
    <h2>Add music</h2>
    <p class="muted">Files are uploaded one at a time. A selected folder keeps its complete folder tree beneath the current SD folder.</p>
    <div class="row">
      <label class="button" for="files">Choose files</label>
      <input id="files" type="file" multiple accept=".flac,.wav,.mp3,.jpg,.jpeg,audio/flac,audio/wav,audio/mpeg,image/jpeg">
      <label class="button" for="folderFiles">Choose folder</label>
      <input id="folderFiles" type="file" multiple webkitdirectory directory accept=".flac,.wav,.mp3,.jpg,.jpeg,audio/flac,audio/wav,audio/mpeg,image/jpeg">
      <button id="clearQueue" type="button">Clear selection</button>
    </div>
    <p id="queueSummary" class="muted">No files selected.</p>
    <ol id="queue" class="queue hidden"></ol>
    <button id="startUpload" class="primary" type="button" disabled>Start upload</button>
  </section>

  <section class="card">
    <h2>Upload progress</h2>
    <div class="meterlabel"><span id="fileProgressText">No active file</span><span id="fileRate">--</span></div>
    <progress id="fileProgress" max="1" value="0"></progress>
    <div class="meterlabel"><span id="overallProgressText">0 / 0 files</span><span id="overallEta">--</span></div>
    <progress id="overallProgress" max="1" value="0"></progress>
  </section>

  <section class="card">
    <h2>Last upload diagnostics</h2>
    <p class="muted">Server-side timings separate network waiting from SD-card writing and final filesystem sync.</p>
    <pre id="uploadDiagnostics" class="diagnostics">No completed upload measured yet.</pre>
  </section>

  <section class="card">
    <h2>Local firmware update</h2>
    <p class="muted">Install an <strong>application-only .bin</strong> from this device. The file stays on your local Wi-Fi connection, settings are preserved, and DSPi restarts only after verification. Do not select a 16 MB Full image.</p>
    <div class="row">
      <label id="firmwarePicker" class="button" for="firmwareFile">Choose application .bin</label>
      <input id="firmwareFile" type="file" accept=".bin,application/octet-stream">
      <button id="installFirmware" class="safe" type="button" disabled>Install firmware</button>
    </div>
    <p id="firmwareSelection" class="muted">No firmware selected.</p>
    <div class="meterlabel"><span id="firmwareProgressText">No update running</span><span id="firmwareRate">--</span></div>
    <progress id="firmwareProgress" max="1" value="0"></progress>
  </section>

  <section class="card">
    <h2>Incomplete uploads</h2>
    <p class="muted">Only temporary files ending in <code>.uploading</code> can be deleted here.</p>
    <div id="incomplete" class="table"></div>
    <div class="row pager">
      <button id="prevIncomplete" type="button">Previous 96</button>
      <span id="incompletePageLabel" class="muted"></span>
      <button id="nextIncomplete" type="button">Next 96</button>
    </div>
  </section>

  <section class="card row actions">
    <button id="cancelUpload" class="danger" type="button" disabled>Cancel current upload</button>
    <button id="finish" class="safe" type="button" disabled>Finish Safely</button>
  </section>
</main>
<script>
"use strict";
const API=Object.freeze({
  status:"/api/status",list:"/api/list",mkdir:"/api/mkdir",
  preflight:"/api/preflight",upload:"/api/upload",cancel:"/api/cancel",
  incomplete:"/api/incomplete",folder:"/api/folder",delete:"/api/delete",finish:"/api/finish",network:"/api/network",
  networkScan:"/api/network/scan",firmware:"/api/firmware"
});
const PAGE_SIZE=96;
const ALLOWED=/\.(flac|wav|mp3|jpe?g)$/i;
const $=id=>document.getElementById(id);
const sleep=ms=>new Promise(resolve=>setTimeout(resolve,ms));
let currentPath="",queueBasePath="",page=0,incompletePage=0,hasMore=false,incompleteHasMore=false,deleting=false;
let queue=[],running=false,cancelRequested=false,controller=null;
let serverWriterActive=false,serverAccepting=false,statusBusy=false,finishBusy=false;
let storageAvailable=null;
let finishingClient=false,statusTimer=0,networkTimer=0,scanningNetwork=false;
let firmwareFile=null,firmwareRunning=false,firmwareStarted=0;
let scannedNetworks=[];
let totalBytes=0,completedBytes=0,batchStarted=0,currentStarted=0,currentIndex=-1;

function numberValue(value){
  const n=Number(value);
  return Number.isFinite(n)&&n>=0?n:0;
}
function signedNumberValue(value){
  const n=Number(value);
  return Number.isFinite(n)?n:0;
}
function formatBytes(value){
  const n=numberValue(value);
  if(n<1024)return n.toFixed(0)+" B";
  const units=["KiB","MiB","GiB","TiB"];
  let v=n/1024,i=0;
  while(v>=1024&&i<units.length-1){v/=1024;i++;}
  return v.toFixed(v>=100?0:v>=10?1:2)+" "+units[i];
}
function formatTime(seconds){
  if(!Number.isFinite(seconds)||seconds<0)return "--";
  if(seconds<60)return Math.ceil(seconds)+"s";
  const m=Math.floor(seconds/60),s=Math.ceil(seconds%60);
  return m+"m "+s+"s";
}
function safeMessage(error,fallback){
  return error&&typeof error.message==="string"?error.message:fallback;
}
function setNotice(text,kind=""){
  const node=$("notice");
  node.textContent=text;
  node.className="card"+(kind?" "+kind:"");
}
async function responseJson(response){
  let data={};
  try{data=await response.json();}catch(_){}
  if(!response.ok||data.ok===false){
    const error=new Error(data.message||data.error||("Request failed ("+response.status+")"));
    error.status=response.status;
    error.reason=data.error||"http_error";
    throw error;
  }
  return data;
}
async function requestJson(url,options={}){
  const response=await fetch(url,Object.assign({cache:"no-store"},options));
  return responseJson(response);
}
function query(route,parameters){
  const parts=[];
  Object.keys(parameters).forEach(key=>parts.push(encodeURIComponent(key)+"="+encodeURIComponent(String(parameters[key]))));
  return route+"?"+parts.join("&");
}
// Encode each component exactly once while preserving real '/' separators.
// Encoding the complete path with encodeURIComponent() would turn separators
// into %2F; firmware correctly rejects encoded separators as a traversal
// ambiguity. A literal '%' in a filename becomes %25 and is decoded once.
function encodePathHeader(path){
  const text=String(path||"");
  return text?text.split("/").map(component=>encodeURIComponent(component)).join("/"):"";
}
function transferHeaders(base,path,size){
  const headers={"X-DSPi-Path":encodePathHeader(path)};
  const encodedBase=encodePathHeader(base);
  if(encodedBase)headers["X-DSPi-Base"]=encodedBase;
  if(size!==undefined&&size!==null)headers["X-DSPi-Declared-Size"]=String(size);
  return headers;
}
function updateControls(){
  const locked=finishingClient||firmwareRunning;
  $("startUpload").disabled=locked||running||deleting||!serverAccepting||queue.length===0;
  $("clearQueue").disabled=locked||running||deleting||queue.length===0;
  $("cancelUpload").disabled=locked||deleting||(!running&&!serverWriterActive);
  // The server also enforces this. The browser never offers Finish while a
  // local queue, request, or authoritative server writer is active.
  $("finish").disabled=locked||finishBusy||running||deleting||serverWriterActive||queue.length>0||!serverAccepting;
  $("saveNetwork").disabled=locked||running||deleting||serverWriterActive||!serverAccepting||scanningNetwork;
  $("scanNetwork").disabled=locked||running||deleting||serverWriterActive||!serverAccepting||scanningNetwork;
  $("wifiFilter").disabled=locked||running||deleting||serverWriterActive||scanningNetwork;
  $("wifiNetworkSelect").disabled=locked||running||deleting||serverWriterActive||scanningNetwork||scannedNetworks.length===0;
  $("firmwareFile").disabled=locked||running||deleting||serverWriterActive||!serverAccepting;
  $("firmwarePicker").classList.toggle("disabled",$("firmwareFile").disabled);
  $("installFirmware").disabled=locked||running||deleting||serverWriterActive||!serverAccepting||!firmwareFile;
  document.querySelectorAll("button.deleteEntry").forEach(button=>{
    button.disabled=locked||running||deleting||serverWriterActive||!serverAccepting;
  });
  // Keep firmware/network controls independent of SD availability.
  ["listing","mkdirForm","files","folderFiles","incomplete"].forEach(id=>{
    const section=$(id).closest("section");
    section.querySelectorAll("button,input").forEach(control=>{
      if(storageAvailable===false)control.disabled=true;
    });
  });
}
async function refreshStatus(showErrors=false){
  if(statusBusy||finishingClient||firmwareRunning||deleting||(running&&!showErrors))return null;
  statusBusy=true;
  try{
    const data=await requestJson(API.status);
    serverWriterActive=Boolean(data.writerActive);
    serverAccepting=Boolean(data.accepting);
    storageAvailable=Boolean(data.storageAvailable);
    $("storageNotice").textContent=storageAvailable?"SD card ready":"No SD card available. Firmware updates still work. Insert a card and reopen Wi-Fi for music transfers.";
    $("mode").textContent=data.mode||"Wi-Fi Transfer";
    $("free").textContent=storageAvailable?formatBytes(data.freeBytes):"No SD card";
    $("writer").textContent=serverWriterActive?(data.currentFile||"Active"):"Idle";
    if(data.message&&!running)setNotice(data.message);
    updateControls();
    return data;
  }catch(error){
    serverAccepting=false;
    if(showErrors)setNotice("Status unavailable: "+safeMessage(error,"connection lost"),"bad");
    updateControls();
    return null;
  }finally{statusBusy=false;}
}
function stopPolling(){
  if(statusTimer){clearInterval(statusTimer);statusTimer=0;}
  if(networkTimer){clearInterval(networkTimer);networkTimer=0;}
}
function startPolling(){
  stopPolling();
  if(finishingClient)return;
  statusTimer=setInterval(()=>refreshStatus(false),2000);
  networkTimer=setInterval(()=>refreshNetwork(false),5000);
}
async function refreshNetwork(showErrors=false){
  if(finishingClient||firmwareRunning||running||deleting)return null;
  try{
    const data=await requestJson(API.network);
    let text="Direct access: http://"+(data.apIp||"192.168.4.1");
    if(data.connected&&data.lanIp){
      text+=" | Home network: http://"+data.lanIp;
      if(data.host)text+=" or http://"+data.host;
    }else if(data.connecting){
      text+=" | Joining "+(data.ssid||"saved Wi-Fi")+"...";
    }else if(data.saved){
      text+=" | Saved network not connected";
    }else{
      text+=" | No home network saved";
    }
    $("networkStatus").textContent=text;
    if(data.ssid&&!$("wifiSsid").value)$("wifiSsid").value=data.ssid;
    renderNetworkChoices();
    return data;
  }catch(error){
    if(showErrors)$("networkStatus").textContent="Network status unavailable: "+safeMessage(error,"connection lost");
    return null;
  }
}
function renderNetworkChoices(){
  const select=$("wifiNetworkSelect");
  const filter=$("wifiFilter").value.trim().toLocaleLowerCase();
  const selected=$("wifiSsid").value;
  select.replaceChildren();
  const placeholder=document.createElement("option");
  placeholder.value="";
  placeholder.textContent=scannedNetworks.length?"Choose a scanned network":"Scan, then choose a network";
  select.append(placeholder);
  let shown=0;
  scannedNetworks.forEach(network=>{
    const ssid=String(network.ssid||"");
    if(!ssid||filter&&!ssid.toLocaleLowerCase().includes(filter))return;
    const option=document.createElement("option");
    option.value=ssid;
    const security=network.secure===false?"open":"secured";
    const channel=numberValue(network.channel);
    option.textContent=ssid+" ("+String(network.rssi||0)+" dBm, "+security+(channel?", ch "+channel:"")+")";
    if(ssid===selected)option.selected=true;
    select.append(option);shown++;
  });
  if(filter&&!shown){
    const empty=document.createElement("option");
    empty.value="";empty.textContent="No scanned network matches this filter";
    select.append(empty);
  }
  updateControls();
}
async function scanNetworks(showErrors=true){
  if(finishingClient||running||serverWriterActive||scanningNetwork)return null;
  scanningNetwork=true;updateControls();
  const wasPolling=Boolean(statusTimer||networkTimer);
  if(wasPolling)stopPolling();
  $("networkStatus").textContent="Scanning nearby Wi-Fi networks...";
  try{
    const data=await requestJson(API.networkScan);
    const seen=new Set();
    const networks=Array.isArray(data.networks)?data.networks:[];
    scannedNetworks=[];
    networks.forEach(network=>{
      const ssid=String(network.ssid||"");
      if(!ssid||seen.has(ssid))return;
      seen.add(ssid);
      scannedNetworks.push(network);
    });
    renderNetworkChoices();
    $("networkStatus").textContent=seen.size?
      (seen.size+" network"+(seen.size===1?"":"s")+" found. Filter or choose from the list."):
      "No nearby Wi-Fi names were found. You can still type the SSID manually.";
    return data;
  }catch(error){
    if(showErrors){
      $("networkStatus").textContent="Wi-Fi scan unavailable: "+safeMessage(error,"request failed");
      setNotice("Wi-Fi scan failed. You can still enter the network name manually.","bad");
    }
    return null;
  }finally{
    scanningNetwork=false;updateControls();
    if(wasPolling&&!finishingClient&&!running)startPolling();
  }
}

async function saveNetwork(event){
  event.preventDefault();
  if(finishingClient||firmwareRunning||running||deleting||serverWriterActive)return;
  const ssid=$("wifiSsid").value;
  const password=$("wifiPassword").value;
  if(!ssid){setNotice("Enter the home Wi-Fi network name.","bad");return;}
  $("saveNetwork").disabled=true;
  try{
    const body=new URLSearchParams({ssid:ssid,password:password});
    await requestJson(API.network,{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body:body.toString()});
    $("wifiPassword").value="";
    setNotice("Wi-Fi details saved. DSPi is connecting while the direct access point stays available.","good");
    await refreshNetwork(true);
  }catch(error){
    setNotice("Wi-Fi details were not saved: "+safeMessage(error,"request failed"),"bad");
  }finally{updateControls();}
}
function clientJoin(base,name){
  return base?base+"/"+name:name;
}
function renderBreadcrumb(){
  const nav=$("crumbs");
  nav.replaceChildren();
  const root=document.createElement("button");
  root.type="button";root.textContent="SD";
  root.addEventListener("click",()=>browse("",0));
  nav.append(root);
  let built="";
  currentPath.split("/").filter(Boolean).forEach(component=>{
    const separator=document.createElement("span");
    separator.className="sep";separator.textContent="/";nav.append(separator);
    built=clientJoin(built,component);
    const target=built;
    const button=document.createElement("button");
    button.type="button";button.textContent=component;
    button.addEventListener("click",()=>browse(target,0));
    nav.append(button);
  });
  $("parent").disabled=!currentPath||running||deleting;
  $("folderLabel").textContent=currentPath?"/"+currentPath:"/";
}
function renderListing(entries){
  const list=$("listing");
  list.replaceChildren();
  const ordered=Array.from(entries||[]).sort((a,b)=>{
    const aDir=a&&(a.type==="dir"||a.directory===true);
    const bDir=b&&(b.type==="dir"||b.directory===true);
    if(aDir!==bDir)return aDir?-1:1;
    return String(a&&a.name||"").localeCompare(
      String(b&&b.name||""),undefined,{numeric:true,sensitivity:"base"});
  });
  if(!ordered.length){
    const empty=document.createElement("p");
    empty.className="muted";empty.style.padding="10px";empty.textContent="This folder is empty.";
    list.append(empty);updateControls();return;
  }
  ordered.forEach(entry=>{
    const row=document.createElement("div");row.className="entry";
    const isDir=entry.type==="dir"||entry.directory===true;
    if(isDir){
      const rawName=String(entry.name||"");
      const target=String(entry.path||clientJoin(currentPath,rawName));
      const button=document.createElement("button");
      button.type="button";button.className="name";
      const icon=document.createElement("span");
      icon.className="folderIcon";icon.setAttribute("aria-hidden","true");
      const label=document.createElement("span");
      label.className="folderText";label.textContent=rawName.replace(/[ .]+$/u,"")||rawName;
      button.append(icon,label);
      const trailing=(rawName.match(/[ .]+$/u)||[""])[0];
      if(trailing){
        const spaces=(trailing.match(/ /g)||[]).length;
        const dots=(trailing.match(/\./g)||[]).length;
        const legacy=document.createElement("span");
        legacy.className="legacyName";
        const parts=[];
        if(spaces)parts.push(spaces+" trailing space"+(spaces===1?"":"s"));
        if(dots)parts.push(dots+" trailing dot"+(dots===1?"":"s"));
        legacy.textContent="["+parts.join(", ")+"]";
        button.append(legacy);
      }
      button.addEventListener("click",()=>browse(target,0));
      const remove=document.createElement("button");
      remove.type="button";remove.className="danger deleteEntry";remove.textContent="Delete";
      remove.setAttribute("aria-label","Delete folder "+(label.textContent||rawName));
      remove.addEventListener("click",()=>deleteListedEntry(target,rawName,"folder"));
      row.append(button,remove);
    }else{
      const rawName=String(entry.name||"");
      const target=String(entry.path||clientJoin(currentPath,rawName));
      const details=document.createElement("div");
      details.className="row";
      const name=document.createElement("span");
      name.className="file";name.textContent=rawName;
      const size=document.createElement("span");
      size.className="size";size.textContent=formatBytes(entry.size);
      details.append(name,size);
      const remove=document.createElement("button");
      remove.type="button";remove.className="danger deleteEntry";remove.textContent="Delete";
      remove.setAttribute("aria-label","Delete file "+rawName);
      remove.addEventListener("click",()=>deleteListedEntry(target,rawName,"file"));
      row.append(details,remove);
    }
    list.append(row);
  });
  updateControls();
}
async function browse(path,newPage=0){
  if(firmwareRunning||running||deleting)return;
  try{
    const data=await requestJson(query(API.list,{path:path,page:newPage,limit:PAGE_SIZE}));
    currentPath=String(data.path||"").replace(/^\/+|\/+$/g,"");
    page=numberValue(data.page);
    hasMore=Boolean(data.hasMore);
    renderBreadcrumb();
    renderListing(Array.isArray(data.entries)?data.entries:[]);
    $("pageLabel").textContent="Page "+(page+1);
    $("prevPage").disabled=page===0;
    $("nextPage").disabled=!hasMore;
    incompletePage=0;
    await loadIncomplete();
  }catch(error){setNotice("Could not open folder: "+safeMessage(error,"request failed"),"bad");}
}
function renderIncomplete(entries){
  const list=$("incomplete");list.replaceChildren();
  if(!entries.length){
    const empty=document.createElement("p");
    empty.className="muted";empty.style.padding="10px";empty.textContent="No incomplete uploads in this folder.";
    list.append(empty);return;
  }
  entries.forEach(entry=>{
    const row=document.createElement("div");row.className="entry";
    const label=document.createElement("span");
    label.className="file";label.textContent=String(entry.name||"")+" ("+formatBytes(entry.size)+")";
    const button=document.createElement("button");
    button.type="button";button.className="danger";button.textContent="Delete incomplete";
    const target=String(entry.path||clientJoin(currentPath,String(entry.name||"")));
    button.addEventListener("click",()=>deleteIncomplete(target));
    row.append(label,button);list.append(row);
  });
}
async function loadIncomplete(){
  try{
    const data=await requestJson(query(API.incomplete,{path:currentPath,page:incompletePage,limit:PAGE_SIZE}));
    incompleteHasMore=Boolean(data.hasMore);
    renderIncomplete(Array.isArray(data.entries)?data.entries:[]);
    $("incompletePageLabel").textContent="Page "+(incompletePage+1);
    $("prevIncomplete").disabled=incompletePage===0;
    $("nextIncomplete").disabled=!incompleteHasMore;
  }catch(error){setNotice("Could not list incomplete uploads: "+safeMessage(error,"request failed"),"bad");}
}
async function deleteIncomplete(path){
  if(running||serverWriterActive)return;
  if(!String(path).toLowerCase().endsWith(".uploading")){
    setNotice("Only .uploading temporary files can be deleted.","bad");return;
  }
  if(!confirm("Delete incomplete file?\n/"+path))return;
  try{
    await requestJson(API.incomplete,{method:"DELETE",headers:transferHeaders("",path)});
    setNotice("Incomplete upload deleted.","good");
    await loadIncomplete();await refreshStatus();
  }catch(error){setNotice("Delete failed: "+safeMessage(error,"request failed"),"bad");}
}
async function deleteListedEntry(path,name,type){
  if(finishingClient||running||deleting||serverWriterActive||!serverAccepting)return;
  const isFolder=type==="folder";
  const shown=isFolder?(String(name||path).replace(/[ .]+$/u,"")||String(name||path)):String(name||path);
  const warning=isFolder
    ?"Permanently delete this folder and everything inside it?"
    :"Permanently delete this file?";
  if(!confirm(warning+"\n\n"+shown+"\n/"+path+"\n\nThis cannot be undone."))return;
  deleting=true;updateControls();
  setNotice(isFolder?"Deleting folder and its contents...":"Deleting file...");
  try{
    const headers=transferHeaders("",path);
    headers["X-DSPi-Confirm"]="DELETE";
    headers["X-DSPi-Entry-Type"]=type;
    const data=await requestJson(API.delete,{method:"POST",headers:headers});
    const count=numberValue(data.removedEntries);
    setNotice(isFolder?("Folder deleted"+(count?" ("+count+" entries removed).":".")):"File deleted.","good");
    deleting=false;updateControls();
    await browse(currentPath,0);await refreshStatus();
  }catch(error){
    deleting=false;updateControls();
    setNotice((isFolder?"Folder":"File")+" delete failed: "+safeMessage(error,"request failed"),"bad");
    await browse(currentPath,0);
  }
}
function pickerPath(file,folderSelection){
  const supplied=folderSelection&&file.webkitRelativePath?file.webkitRelativePath:file.name;
  return String(supplied);
}
function validateClientPath(path){
  if(!path||path.startsWith("/")||path.includes("\\")||path.includes("\0"))return false;
  const parts=path.split("/");
  return !parts.some(part=>!part||part==="."||part==="..");
}
function selectFiles(fileList,folderSelection){
  if(firmwareRunning||running||deleting)return;
  const accepted=[],rejected=[];
  Array.from(fileList).forEach(file=>{
    const relativePath=pickerPath(file,folderSelection);
    if(!validateClientPath(relativePath)||!ALLOWED.test(relativePath)){
      rejected.push(relativePath||"(empty name)");
    }else accepted.push({file:file,relativePath:relativePath});
  });
  accepted.sort((a,b)=>a.relativePath.localeCompare(b.relativePath,undefined,{numeric:true,sensitivity:"base"}));
  queue=accepted;
  queueBasePath=currentPath;
  renderQueue();
  if(rejected.length){
    const sample=rejected.slice(0,3).join(", ");
    setNotice("Skipped "+rejected.length+" unsupported or unsafe item"+(rejected.length===1?"":"s")+": "+sample+(rejected.length>3?"...":""),"bad");
  }else if(queue.length)setNotice(queue.length+" file"+(queue.length===1?"":"s")+" ready. Review the destination, then Start upload.");
  updateControls();
}
function renderQueue(){
  const list=$("queue");list.replaceChildren();
  totalBytes=queue.reduce((sum,item)=>sum+numberValue(item.file.size),0);
  if(!queue.length){
    list.classList.add("hidden");$("queueSummary").textContent="No files selected.";
  }else{
    list.classList.remove("hidden");
    queue.forEach(item=>{
      const li=document.createElement("li");
      li.textContent=item.relativePath+" - "+formatBytes(item.file.size);list.append(li);
    });
    $("queueSummary").textContent=queue.length+" file"+(queue.length===1?"":"s")+", "+formatBytes(totalBytes)+", destination "+(queueBasePath?"/"+queueBasePath:"/");
  }
  updateControls();
}
function renderProgress(fileLoaded){
  const loaded=Math.min(numberValue(fileLoaded),currentIndex>=0?numberValue(queue[currentIndex].file.size):0);
  const fileSize=currentIndex>=0?numberValue(queue[currentIndex].file.size):0;
  const now=performance.now();
  const fileSeconds=Math.max((now-currentStarted)/1000,.001);
  const batchSeconds=Math.max((now-batchStarted)/1000,.001);
  const fileSpeed=loaded/fileSeconds;
  const overallLoaded=Math.min(completedBytes+loaded,totalBytes);
  const overallSpeed=overallLoaded/batchSeconds;
  const remaining=Math.max(totalBytes-overallLoaded,0);
  $("fileProgress").value=fileSize?loaded/fileSize:0;
  $("overallProgress").value=totalBytes?overallLoaded/totalBytes:0;
  const item=currentIndex>=0?queue[currentIndex]:null;
  $("fileProgressText").textContent=item?("File "+(currentIndex+1)+" / "+queue.length+": "+formatBytes(loaded)+" / "+formatBytes(fileSize)):"No active file";
  $("fileRate").textContent=fileSpeed?((fileSpeed/1000000).toFixed(2)+" MB/s, "+formatTime(fileSeconds)):"--";
  $("overallProgressText").textContent=(currentIndex<0?0:currentIndex)+" completed / "+queue.length+" files, "+formatBytes(overallLoaded)+" / "+formatBytes(totalBytes);
  $("overallEta").textContent=overallSpeed?(formatTime(remaining/overallSpeed)+" remaining"):"--";
}
function renderServerDiagnostics(data){
  const p=data&&data.performance?data.performance:null;
  if(!p){$("uploadDiagnostics").textContent="The server returned no throughput telemetry.";return;}
  const total=Math.max(numberValue(p.serverElapsedMs),1);
  const percent=value=>(100*numberValue(value)/total).toFixed(1)+"%";
  const lines=[
    "Server: "+(numberValue(p.serverRateBps)/1000000).toFixed(2)+" MB/s over "+formatTime(total/1000),
    "Network wait: "+numberValue(p.networkWaitMs)+" ms ("+percent(p.networkWaitMs)+")",
    "SD writes: "+numberValue(p.sdWriteMs)+" ms ("+percent(p.sdWriteMs)+"), "+numberValue(p.sdWriteCount)+" writes at "+(numberValue(p.sdWriteRateBps)/1000000).toFixed(2)+" MB/s",
    "File sync: "+numberValue(p.fileSyncMs)+" ms; finalise: "+numberValue(p.finalizeMs)+" ms",
    "Raw HTTP chunks: "+numberValue(p.rawCallbacks)+", average "+formatBytes(p.rawChunkAverageBytes)+", max "+formatBytes(p.rawChunkMaxBytes),
    "Copy: "+numberValue(p.copyMs)+" ms; SPI-lock wait: "+numberValue(p.spiLockWaitMs)+" ms; scheduler yield: "+numberValue(p.yieldMs)+" ms",
    "Pipeline: "+(p.pipelineEnabled?(numberValue(p.pipelineBuffers)+" DMA buffers"):"synchronous fallback")+", "+numberValue(p.pipelineQueuedBlocks)+" blocks, queue wait "+numberValue(p.pipelineQueueWaitMs)+" ms (max "+numberValue(p.pipelineQueueWaitMaxUs)+" us), backpressure "+numberValue(p.pipelineBackpressureCount),
    "Preallocation: "+(p.preallocated?"yes":"fallback")+", "+numberValue(p.preallocateMs)+" ms; final drain "+numberValue(p.pipelineBarrierWaitMs)+" ms",
    "Wi-Fi: channel "+numberValue(p.wifiChannel)+", RSSI "+signedNumberValue(p.wifiRssiDbm)+" dBm, AP clients "+numberValue(p.apClients)
  ];
  $("uploadDiagnostics").textContent=lines.join("\n");
}
function rawPut(file,relativePath,signal,onProgress){
  // queueBasePath is captured when the picker selection is made, so later
  // browsing cannot silently redirect an already reviewed upload queue.
  const url=API.upload;
  return new Promise((resolve,reject)=>{
    const xhr=new XMLHttpRequest();
    let settled=false;
    const finish=(callback,value)=>{
      if(settled)return;settled=true;
      signal.removeEventListener("abort",abortRequest);
      callback(value);
    };
    const abortRequest=()=>xhr.abort();
    xhr.open("PUT",url,true);
    xhr.setRequestHeader("Content-Type","application/octet-stream");
    const headers=transferHeaders(queueBasePath,relativePath,file.size);
    Object.keys(headers).forEach(name=>xhr.setRequestHeader(name,headers[name]));
    xhr.upload.onprogress=event=>onProgress(event.loaded,event.lengthComputable?event.total:file.size);
    xhr.onload=()=>{
      let data={};
      try{data=JSON.parse(xhr.responseText||"{}");}catch(_){}
      if(xhr.status>=200&&xhr.status<300&&data.ok!==false)finish(resolve,data);
      else finish(reject,new Error(data.message||data.error||("Upload failed ("+xhr.status+")")));
    };
    xhr.onerror=()=>finish(reject,new Error("Network connection lost during upload."));
    xhr.onabort=()=>{
      const error=new Error("Upload cancelled.");
      error.name="AbortError";finish(reject,error);
    };
    signal.addEventListener("abort",abortRequest,{once:true});
    if(signal.aborted){abortRequest();return;}
    xhr.send(file);
  });
}
async function uploadOne(item,index){
  currentIndex=index;currentStarted=performance.now();
  controller=new AbortController();
  renderProgress(0);updateControls();
  setNotice("Checking destination for "+item.relativePath+"...");
  await requestJson(API.preflight,{
    method:"POST",headers:transferHeaders(queueBasePath,item.relativePath,item.file.size)
  });
  setNotice("Uploading "+item.relativePath+"...");
  const data=await rawPut(item.file,item.relativePath,controller.signal,loaded=>renderProgress(loaded));
  renderServerDiagnostics(data);
  renderProgress(item.file.size);
  if(numberValue(data.bytes)!==0&&numberValue(data.bytes)!==numberValue(item.file.size)){
    throw new Error("Server byte-count verification did not match the selected file.");
  }
  completedBytes+=numberValue(item.file.size);
  controller=null;
}
async function runQueue(){
  if(running||!queue.length||!serverAccepting)return;
  running=true;cancelRequested=false;completedBytes=0;batchStarted=performance.now();currentIndex=-1;
  // ESP32 WebServer serves one client at a time. Suspend status/network GETs
  // so the raw PUT owns the connection and radio for the duration of a file.
  stopPolling();
  const selectedCount=queue.length;
  updateControls();
  try{
    // Deliberately sequential: one awaited raw PUT and therefore one server
    // writer at a time. Do not replace this with Promise.all().
    for(let i=0;i<queue.length;i++){
      if(cancelRequested)throw new DOMException("Upload cancelled.","AbortError");
      await uploadOne(queue[i],i);
    }
    setNotice(selectedCount+" file"+(selectedCount===1?"":"s")+" uploaded and finalised safely.","good");
  }catch(error){
    if(cancelRequested||error.name==="AbortError")setNotice("Upload cancelled. Any partial file remains marked .uploading.","bad");
    else setNotice("Upload stopped: "+safeMessage(error,"request failed")+". Any partial file remains marked .uploading.","bad");
  }finally{
    controller=null;running=false;currentIndex=-1;
    queue=[];renderQueue();updateControls();
    await waitForWriterIdle();
    await refreshStatus();
    await browse(currentPath,0);
    startPolling();
  }
}
async function waitForWriterIdle(){
  for(let attempt=0;attempt<20;attempt++){
    const status=await refreshStatus();
    if(status&&!status.writerActive)return true;
    await sleep(250);
  }
  return false;
}
async function cancelCurrent(){
  if(!running&&!serverWriterActive)return;
  cancelRequested=true;
  setNotice("Cancelling current upload and closing its temporary file...");
  if(controller)controller.abort();
  try{await requestJson(API.cancel,{method:"POST"});}catch(_){}
  await waitForWriterIdle();
  updateControls();
}
function selectFirmware(fileList){
  if(firmwareRunning)return;
  const selected=fileList&&fileList.length?fileList[0]:null;
  if(!selected){firmwareFile=null;$("firmwareSelection").textContent="No firmware selected.";updateControls();return;}
  if(!/\.bin$/i.test(selected.name)){
    firmwareFile=null;
    $("firmwareSelection").textContent="Choose an application-only file ending in .bin.";
    setNotice("Firmware selection rejected: only an application .bin is accepted.","bad");
  }else{
    firmwareFile=selected;
    $("firmwareSelection").textContent=selected.name+" - "+formatBytes(selected.size);
    setNotice("Firmware selected. Confirm it is an application-only image, then choose Install firmware.");
  }
  updateControls();
}
async function installFirmware(){
  if(!firmwareFile||firmwareRunning||running||deleting||serverWriterActive||!serverAccepting)return;
  const selected=firmwareFile;
  const warning="Install "+selected.name+" ("+formatBytes(selected.size)+")?\n\nUse only an application-only .bin, never a 16 MB Full image. Settings will be preserved. Keep power connected until DSPi restarts.";
  if(!window.confirm(warning))return;
  firmwareRunning=true;firmwareStarted=performance.now();stopPolling();updateControls();
  $("firmwareProgress").value=0;
  $("firmwareProgressText").textContent="Starting local firmware upload...";
  $("firmwareRate").textContent="--";
  setNotice("Uploading and verifying firmware. Keep this page open and do not remove power.");
  try{
    const data=await new Promise((resolve,reject)=>{
      const xhr=new XMLHttpRequest();
      xhr.open("PUT",API.firmware,true);
      xhr.setRequestHeader("Content-Type","application/octet-stream");
      xhr.setRequestHeader("X-DSPi-Declared-Size",String(selected.size));
      xhr.upload.onprogress=event=>{
        const loaded=numberValue(event.loaded),elapsed=Math.max((performance.now()-firmwareStarted)/1000,.001);
        $("firmwareProgress").value=selected.size?Math.min(loaded/selected.size,1):0;
        $("firmwareProgressText").textContent=formatBytes(loaded)+" / "+formatBytes(selected.size);
        $("firmwareRate").textContent=(loaded/elapsed/1000000).toFixed(2)+" MB/s";
      };
      xhr.onload=()=>{
        let body={};try{body=JSON.parse(xhr.responseText||"{}");}catch(_){}
        if(xhr.status>=200&&xhr.status<300&&body.ok!==false)resolve(body);
        else reject(new Error(body.message||body.error||("Firmware update failed ("+xhr.status+")")));
      };
      xhr.onerror=()=>reject(new Error("Local connection was lost during firmware upload."));
      xhr.onabort=()=>reject(new Error("Firmware upload was cancelled."));
      xhr.send(selected);
    });
    $("firmwareProgress").value=1;
    $("firmwareProgressText").textContent="Verified "+formatBytes(selected.size);
    $("firmwareRate").textContent="Restarting";
    finishingClient=true;
    setNotice((data.message||"Firmware verified. DSPi is restarting.")+" The local page will disconnect; this is expected.","good");
  }catch(error){
    firmwareRunning=false;
    setNotice("Firmware was not installed: "+safeMessage(error,"request failed"),"bad");
    $("firmwareProgressText").textContent="Update failed; the current firmware remains bootable.";
    await refreshStatus(false);startPolling();updateControls();
  }
}
async function finishSafely(){
  if(firmwareRunning||finishBusy||running||deleting||serverWriterActive||queue.length)return;
  finishBusy=true;updateControls();
  try{
    // Re-read authoritative state immediately before requesting shutdown.
    const status=await refreshStatus(true);
    if(!status||status.writerActive)throw new Error("An upload writer is still active.");
    finishingClient=true;
    stopPolling();
    updateControls();
    setNotice("Finishing safely: syncing, restoring normal SD access and closing Wi-Fi. The control panel will return to Music automatically...");
    const data=await requestJson(API.finish,{method:"POST"});
    serverAccepting=false;
    setNotice(data.message||"Finish accepted. Wait for the DSPi screen to return to Music before removing power.","good");
  }catch(error){
    finishingClient=false;
    setNotice("Could not finish safely: "+safeMessage(error,"request failed"),"bad");
    await refreshStatus();
    startPolling();
  }finally{finishBusy=false;updateControls();}
}
$("refresh").addEventListener("click",()=>browse(currentPath,page));
$("parent").addEventListener("click",()=>{
  const parts=currentPath.split("/").filter(Boolean);parts.pop();browse(parts.join("/"),0);
});
$("prevPage").addEventListener("click",()=>browse(currentPath,Math.max(0,page-1)));
$("nextPage").addEventListener("click",()=>{if(hasMore)browse(currentPath,page+1);});
$("prevIncomplete").addEventListener("click",()=>{if(incompletePage>0){incompletePage--;loadIncomplete();}});
$("nextIncomplete").addEventListener("click",()=>{if(incompleteHasMore){incompletePage++;loadIncomplete();}});
$("mkdirForm").addEventListener("submit",async event=>{
  event.preventDefault();
  if(deleting||running||serverWriterActive||!serverAccepting)return;
  const relative=$("folderName").value.trim();
  if(!relative)return;
  try{
    await requestJson(API.mkdir,{method:"POST",headers:transferHeaders(currentPath,relative)});
    $("folderName").value="";setNotice("Folder created.","good");await browse(currentPath,0);
  }catch(error){setNotice("Folder was not created: "+safeMessage(error,"request failed"),"bad");}
});
$("files").addEventListener("change",event=>{selectFiles(event.target.files,false);event.target.value="";});
$("folderFiles").addEventListener("change",event=>{selectFiles(event.target.files,true);event.target.value="";});
$("clearQueue").addEventListener("click",()=>{if(!running){queue=[];renderQueue();setNotice("Selection cleared.");}});
$("startUpload").addEventListener("click",runQueue);
$("cancelUpload").addEventListener("click",cancelCurrent);
$("finish").addEventListener("click",finishSafely);
$("scanNetwork").addEventListener("click",()=>scanNetworks(true));
$("wifiFilter").addEventListener("input",renderNetworkChoices);
$("wifiNetworkSelect").addEventListener("change",event=>{
  const selected=String(event.target.value||"");
  if(selected)$("wifiSsid").value=selected;
  renderNetworkChoices();
});
$("wifiSsid").addEventListener("input",renderNetworkChoices);
$("networkForm").addEventListener("submit",saveNetwork);
$("firmwareFile").addEventListener("change",event=>{selectFirmware(event.target.files);event.target.value="";});
$("installFirmware").addEventListener("click",installFirmware);
window.addEventListener("beforeunload",event=>{
  if(firmwareRunning||running||serverWriterActive){event.preventDefault();event.returnValue="";}
});

(async()=>{
  renderBreadcrumb();renderQueue();renderProgress(0);
  const status=await refreshStatus(true);
  const network=await refreshNetwork(true);
  // Do not interrupt an already-saved station join with an automatic active
  // scan.  New installations still get an immediate list; existing users can
  // rescan explicitly with the button whenever they need to change network.
  if(!network||!network.saved)await scanNetworks(false);
  if(status){setNotice(storageAvailable?(status.message||"Ready. Choose an SD folder and select files."):"Update-only mode: choose an application .bin below. No SD card required.");if(storageAvailable)await browse("",0);}
  startPolling();
})();
</script>
</body>
</html>
)DSPITRANSFER";

}  // namespace WifiTransferWeb
