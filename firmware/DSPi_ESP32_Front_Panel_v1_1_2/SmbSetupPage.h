#pragma once
namespace SmbLibrary {
const char setupPage[]=R"HTML(<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>DSPi Network Music</title>
<style>body{font:18px system-ui;background:#17191c;color:#eee;max-width:32em;margin:1em auto;padding:1em}label{display:block;margin:.7em 0}input,select,button{box-sizing:border-box;width:100%;font:inherit;padding:.6em;margin-top:.3em}button{background:#1686e8;color:white;border:0}small{color:#bbb}hr{margin:1.5em 0}</style>
<h1>Network Music</h1><p>Stay on the setup Wi-Fi until finished. Windows/NAS shares use SMB2/3. This panel only reads files.</p>
<details id="wifi"><summary>Home Wi-Fi</summary><select id="networks"><option value="">Select network</option></select><button id="scan">Scan again</button><form id="wf"><label>Network name<input id="ssid" maxlength="32" required autocapitalize="none"></label><label>Wi-Fi password<input id="wp" type="password" maxlength="63"></label><button>Connect</button></form></details>
<p id="ws" role="status">Checking saved Wi-Fi…</p><hr>
<form id="sf"><label>Saved share<select id="slot"></select></label><label>Name<input id="label" maxlength="32" required></label><label>Server<input id="server" maxlength="253" placeholder="192.168.1.146 or NAS name" required autocapitalize="none" spellcheck="false"></label><label>Share<input id="share" maxlength="127" placeholder="Music" required></label><label>Starting folder (optional)<input id="root" maxlength="256" placeholder="FLAC" autocapitalize="none"></label><label>Username (blank for guest)<input id="user" maxlength="127" autocomplete="off" autocapitalize="none"></label><label>Password<input id="pass" type="password" maxlength="127" autocomplete="new-password"></label><label><input id="keep" type="checkbox" style="width:auto" checked> Keep saved password when blank</label><label>Domain (optional)<input id="domain" maxlength="127" autocapitalize="none"></label><button id="save">Test connection and save</button></form><button id="remove">Remove share</button><p id="result" role="status"></p><button id="done">Finish</button>
<script>
const token=new URLSearchParams(location.hash.slice(1)).get('setup')||sessionStorage.getItem('dspiSetup')||'';
if(token.length===32)sessionStorage.setItem('dspiSetup',token);history.replaceState(null,'',location.pathname);
const el=id=>document.getElementById(id);let rows=[],busy=false,connected=false,lastState='';
const endpoint='/dspi/panel/smb/'+token;
async function put(data){return fetch('/dspi/panel/network',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({token,...data})})}
function fill(){const r=rows[+el('slot').value]||{};for(const k of ['label','server','share','root','user','domain'])el(k).value=r[k]||'';el('pass').value='';el('keep').checked=!!r.hasPassword}
el('slot').onchange=fill;
async function shares(update=false){try{const r=await fetch(endpoint,{cache:'no-store'});if(!r.ok)throw Error();const d=await r.json();rows=d.shares;if(!el('slot').options.length){rows.forEach((s,i)=>el('slot').add(new Option(s.label||'Empty '+(i+1),i)));fill()}else if(update){rows.forEach((s,i)=>el('slot').options[i].text=s.label||'Empty '+(i+1));fill()}
 busy=d.state==='checking';el('save').disabled=busy||!connected;el('remove').disabled=busy;el('done').disabled=busy;
 el('result').textContent=({checking:'Checking share…',saved:'Saved. Close setup to browse on the panel.',failed:'Could not connect or save. Previous shares kept. Check server, share, folder and login.',invalid:'Invalid share settings.',unavailable:'Share storage unavailable.'})[d.state]||'';
 if(d.state==='saved'&&!update&&lastState!=='saved'){lastState='saved';await shares(true)}else lastState=d.state;
 }catch(e){el('result').textContent='Rejoin setup Wi-Fi or scan the panel page QR again.'}}
async function status(){try{const r=await fetch('/dspi/panel/setup-status',{cache:'no-store'});const d=await r.json();connected=d.state==='saved';el('ws').textContent=connected?'Home Wi-Fi connected. Configure a share below.':d.state==='joining'?'Connecting to home Wi-Fi…':'Choose home Wi-Fi above.';if(!connected&&d.state!=='joining')el('wifi').open=true;await shares()}catch(e){}setTimeout(status,1200)}
async function scan(){try{let d;for(let n=0;n<20;n++){const r=await fetch(n?'/dspi/panel/networks':'/dspi/panel/rescan',{cache:'no-store'});d=await r.json();if(d.state!=='scanning')break;await new Promise(r=>setTimeout(r,750))}el('networks').replaceChildren(new Option('Select network',''));for(const r of d.networks||[])el('networks').add(new Option(r.ssid,r.ssid))}catch(e){el('ws').textContent='Enter Wi-Fi name manually.'}}
el('scan').onclick=scan;el('networks').onchange=()=>el('ssid').value=el('networks').value;
el('wf').onsubmit=async e=>{e.preventDefault();const r=await put({ssid:el('ssid').value,password:el('wp').value});el('wp').value='';el('ws').textContent=r.status===202?'Connecting…':'Request refused. Reopen setup if needed.'};
el('sf').onsubmit=async e=>{e.preventDefault();if(busy||!connected)return;const data={action:'save',slot:+el('slot').value,keepPassword:el('keep').checked,password:el('pass').value};for(const k of ['label','server','share','root','user','domain'])data[k]=el(k).value;busy=true;lastState='';el('save').disabled=true;try{const r=await put(data);if(r.status!==202)el('result').textContent='Invalid details or setup busy.'}finally{el('pass').value='';setTimeout(()=>shares(),300)}};
el('remove').onclick=async()=>{if(busy||!confirm('Remove this saved share?'))return;busy=true;lastState='';await put({action:'remove',slot:+el('slot').value});setTimeout(()=>shares(true),500)};
el('done').onclick=async()=>{if(busy)return;const r=await put({done:true});if(r.ok){sessionStorage.removeItem('dspiSetup');document.body.textContent='Setup finished. Reconnect to home Wi-Fi and use Network on the panel.'}};
status();
</script></html>)HTML";
}
