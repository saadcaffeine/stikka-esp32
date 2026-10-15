#include "web_ui.h"

#include <Update.h>
#include <cstring>
#include <new>
#include <WebServer.h>
#include <WiFi.h>
#include <StreamString.h>

#include "config.h"
#include "logging.h"
#include "mqtt_bridge.h"
#include "print_queue.h"
#include "targets/target.h"
#include "test_print.h"
#include "wifi_manager.h"

static WebServer web(80);

static String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length() + 16);
  for (size_t i = 0; i < in.length(); i++) {
    const char c = in[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }
  return out;
}

static uint16_t parsePort(const String& value, uint16_t fallback) {
  const long p = value.toInt();
  if (p <= 0 || p > 65535) return fallback;
  return (uint16_t)p;
}

// Protocol+method combinations actually implemented *and* tested. The
// firmware itself can run other combinations (serial method -- see
// targets/serial_target.cpp), but they haven't been validated against real
// hardware yet, so the web UI deliberately doesn't offer them. Add a row
// here once a new combination has been confirmed working.
//
// seiko_usb_host is here under active hardware validation: unlike
// ql_usb_host (confirmed against a Brother QL-720NW, which identifies as a
// USB Printer-class interface, bInterfaceClass 0x07), Seiko SLP-series
// printers commonly expose a vendor-specific USB class instead, so this
// exercises usb_host_target.cpp's fallback bulk-OUT-endpoint claim path
// (see fallbackEndpointFound et al there) rather than the primary
// printer-class scan. If that path turns out not to match real Seiko
// hardware, expect to be back in usb_host_target.cpp's descriptor walk, not
// here.
struct PrinterTypeOption {
  const char* value;     // posted <select> value, "<protocol>_<method>"
  const char* protocol;  // -> cfg.printerProtocol
  const char* method;    // -> cfg.printerMethod
  const char* label;
};

static const PrinterTypeOption kPrinterTypeOptions[] = {
    {"zpl_network", "zpl", "network", "Zebra / generic ZPL &mdash; Network (TCP)"},
    {"zpl_usb_host", "zpl", "usb_host", "Zebra / generic ZPL &mdash; USB (printer plugged into this board)"},
    {"ql_usb_host", "ql", "usb_host", "Brother QL &mdash; USB (printer plugged into this board)"},
    {"seiko_usb_host", "seiko", "usb_host", "Seiko SLP &mdash; USB (printer plugged into this board)"},
};
static const size_t kPrinterTypeOptionCount = sizeof(kPrinterTypeOptions) / sizeof(kPrinterTypeOptions[0]);

// Looks up a posted 'printerType' value against kPrinterTypeOptions. Returns
// false (leaving protocolOut/methodOut untouched) for anything else -- an
// empty/missing field (fresh device) or a stale value from before this
// dropdown was narrowed down (a device previously configured for serial or
// seiko), so callers can fall back to their own default.
static bool printerTypeFromValue(const String& value, String& protocolOut, String& methodOut) {
  for (size_t i = 0; i < kPrinterTypeOptionCount; i++) {
    if (value == kPrinterTypeOptions[i].value) {
      protocolOut = kPrinterTypeOptions[i].protocol;
      methodOut = kPrinterTypeOptions[i].method;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Shared look & feel. Mobile-first, no external assets, no frameworks: one
// small stylesheet + one small script inlined into every page (so it also
// works from the captive-portal fallback AP with no internet). Colour carries
// meaning: blue = Wi-Fi, violet = MQTT, teal = printer, amber = label/caution,
// slate = device/debug, red = destructive. Buttons visibly change state
// (idle -> busy spinner -> done / failed) via btn() in kJs, and the header
// pills poll /status.json so connection state is always on screen.
// ---------------------------------------------------------------------------
static const char kCss[] = R"CSS(
:root{--bg:#f1f5f9;--ink:#0f172a;--mute:#64748b;--line:#e2e8f0;--blue:#2563eb;--green:#16a34a;--amber:#d97706;--red:#dc2626;--violet:#7c3aed;--teal:#0d9488;--slate:#475569}
*{box-sizing:border-box}[hidden]{display:none!important}
body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
header{position:sticky;top:0;z-index:5;background:#0f172a;color:#fff;padding:.6rem 1rem .7rem}
.top{display:flex;align-items:baseline;justify-content:space-between;gap:.5rem}
.top b{font-size:1.05rem}.top span{color:#94a3b8;font-size:.85rem;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
nav{display:flex;gap:.3rem;margin-top:.5rem}
nav a{flex:1;text-align:center;padding:.4rem 0;border-radius:8px;color:#cbd5e1;text-decoration:none;font-weight:600;font-size:.9rem}
nav a.on{background:#334155;color:#fff}
.pills{display:flex;gap:.4rem;flex-wrap:wrap;margin-top:.55rem}
.pill{display:inline-flex;align-items:center;gap:.4rem;font-size:.75rem;padding:.15rem .6rem;border-radius:99px;background:#1e293b;color:#e2e8f0}
.pill i{width:.55rem;height:.55rem;border-radius:50%;background:#64748b;flex:none}
.pill.ok i{background:#22c55e}.pill.wait i{background:#f59e0b}.pill.bad i{background:#ef4444}
.pill.lt{background:#e2e8f0;color:var(--ink)}
main{max-width:720px;margin:0 auto;padding:1rem 1rem 6.5rem}
h1{font-size:1.3rem;margin:.2rem 0}.sub{color:var(--mute);margin:0 0 1rem;font-size:.9rem}
.card{background:#fff;border:1px solid var(--line);border-top:4px solid var(--c,var(--slate));border-radius:12px;padding:1rem;margin-bottom:1rem}
.card h2{font-size:1rem;margin:0 0 .25rem;color:var(--c,var(--slate))}
.card h3{font-size:.85rem;margin:1rem 0 .1rem;color:var(--mute)}
.wifi{--c:var(--blue)}.mqtt{--c:var(--violet)}.prn{--c:var(--teal)}.lbl{--c:var(--amber)}.dev{--c:var(--slate)}.danger{--c:var(--red)}
label{display:block;font-weight:600;font-size:.88rem;margin-top:.85rem}
input,select,textarea{display:block;width:100%;margin-top:.3rem;padding:.65rem .7rem;font:inherit;font-weight:400;color:inherit;background:#fff;border:1px solid #cbd5e1;border-radius:8px}
input:focus,select:focus,textarea:focus{outline:2px solid var(--c,var(--blue));outline-offset:1px;border-color:transparent}
label.chk{display:flex;gap:.6rem;align-items:flex-start;font-weight:500}
label.chk input{width:1.15rem;height:1.15rem;margin:.2rem 0 0;flex:none}
small{display:block;color:var(--mute);font-size:.8rem;margin-top:.4rem}
small:empty{display:none}
code{background:#f1f5f9;padding:.05rem .3rem;border-radius:4px;font-size:.85em;word-break:break-all}
a{color:var(--blue)}
.kv{display:flex;justify-content:space-between;gap:1rem;padding:.4rem 0;border-bottom:1px solid var(--line);font-size:.9rem}
.kv:last-child{border:0}.kv span{color:var(--mute);flex:none}.kv b{font-weight:600;text-align:right;word-break:break-word}
.row{display:flex;gap:.5rem;flex-wrap:wrap;margin-top:.7rem}
.cols{display:grid;grid-template-columns:repeat(auto-fit,minmax(7.5rem,1fr));gap:.6rem}
.btn{display:inline-flex;align-items:center;justify-content:center;gap:.5rem;min-height:2.8rem;padding:.5rem 1rem;border:0;border-radius:10px;font:inherit;font-weight:600;color:#fff;background:var(--b,var(--blue));cursor:pointer;transition:background .15s,transform .05s}
.btn:hover{filter:brightness(1.08)}.btn:active{transform:scale(.98)}
.btn.block{width:100%;margin-top:.8rem}.btn.sm{min-height:2.1rem;padding:.2rem .75rem;font-size:.85rem;border-radius:8px}
.btn.green{--b:var(--green)}.btn.amber{--b:var(--amber)}.btn.red{--b:var(--red)}.btn.slate{--b:var(--slate)}.btn.teal{--b:var(--teal)}
.btn.ghost{background:transparent;color:var(--b,var(--blue));box-shadow:inset 0 0 0 2px var(--b,var(--blue))}
.btn:disabled{cursor:not-allowed;opacity:.55;filter:grayscale(.5)}
.btn.busy,.btn.ok,.btn.err{background:var(--b);color:#fff;box-shadow:none;opacity:1;filter:none}
.btn.busy{--b:var(--slate);cursor:progress}.btn.ok{--b:var(--green)}.btn.err{--b:var(--red)}
.btn.busy::before{content:"";width:1em;height:1em;border:2px solid #fff6;border-top-color:#fff;border-radius:50%;animation:sp .8s linear infinite}
@keyframes sp{to{transform:rotate(360deg)}}
@media(prefers-reduced-motion:reduce){.btn.busy::before{animation:none}}
.net{display:flex;align-items:center;gap:.4rem;padding:.4rem 0;border-bottom:1px solid var(--line)}
.net span{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-size:.9rem}
.bar{position:fixed;left:0;right:0;bottom:0;z-index:5;background:#fff;border-top:1px solid var(--line);padding:.6rem 1rem calc(.6rem + env(safe-area-inset-bottom))}
.bar div{max-width:720px;margin:0 auto;display:flex;gap:.8rem;align-items:center}
.bar .btn{--b:var(--slate);flex:1}.bar.dirty .btn{--b:var(--green)}
.bar .btn.busy{--b:var(--slate)}.bar .btn.ok{--b:var(--green)}.bar .btn.err{--b:var(--red)}
.dirty-t{font-size:.8rem;color:var(--mute);flex:none;max-width:40%}.bar.dirty .dirty-t{color:var(--amber);font-weight:600}
#toast{position:fixed;left:1rem;right:1rem;bottom:5rem;max-width:688px;margin:0 auto;padding:.7rem 1rem;border-radius:10px;color:#fff;background:var(--slate);font-size:.9rem;display:none;z-index:6;box-shadow:0 4px 14px #0003;word-break:break-word}
#toast.show{display:block}#toast.ok{background:var(--green)}#toast.err{background:var(--red)}#toast.wait{background:var(--amber)}
progress{width:100%;height:.7rem;margin-top:.8rem;accent-color:var(--amber)}
.tools{display:flex;gap:.8rem;align-items:center;flex-wrap:wrap}.tools label{margin:0;display:flex;align-items:center;gap:.4rem;font-weight:500}.tools input,.tools select{width:auto;margin:0}
#log{background:#0f172a;color:#cbd5e1;border-radius:12px;padding:.75rem;height:62vh;overflow-y:auto;font:.8rem/1.45 ui-monospace,Consolas,Menlo,monospace;white-space:pre-wrap;word-break:break-word}
.lvl-ERROR{color:#f87171}.lvl-WARN{color:#fbbf24}.lvl-INFO{color:#7dd3fc}.lvl-DEBUG{color:#94a3b8}
)CSS";

static const char kJs[] = R"JS(
var $=function(s){return document.querySelector(s)};
function btn(b,s,t){if(!b)return;if(!b.dataset.l)b.dataset.l=b.textContent;clearTimeout(b._t);
b.classList.remove('busy','ok','err');
if(s==='idle'){b.disabled=false;b.textContent=b.dataset.l;return;}
b.classList.add(s);b.disabled=(s==='busy');b.textContent=t||b.dataset.l;
if(s!=='busy')b._t=setTimeout(function(){btn(b,'idle')},2500);}
var tt;function toast(m,k){var t=$('#toast');if(!t)return;t.textContent=m;t.className='show '+(k||'');clearTimeout(tt);if(k!=='wait')tt=setTimeout(function(){t.className=''},6000);}
function pill(id,c,x){var p=$('#p-'+id);if(p){p.className='pill '+c+(p.dataset.lt?' lt':'');p.lastChild.textContent=x;}}
function status(){return fetch('/status.json',{cache:'no-store'}).then(function(r){return r.json()}).then(function(s){
pill('wifi',s.wifi?'ok':(s.ap?'wait':'bad'),s.wifi?'Wi-Fi '+s.ip:(s.ap?'Setup AP':'Wi-Fi off'));
pill('mqtt',s.mqtt?'ok':'bad',s.mqtt?'MQTT on':'MQTT off');
if(s.job==='printing')pill('prn','wait','Printing'+(s.pct>=0?' '+s.pct+'%':'')+(s.queue?' (+'+s.queue+' queued)':''));
else if(s.last_error)pill('prn','bad','Last print failed');
else pill('prn',s.printer==='ready'?'ok':(s.printer==='missing'?'bad':'wait'),s.printer==='ready'?'Printer ready':(s.printer==='missing'?'No printer':'Printer: '+s.method));
var pp=$('#p-prn');if(pp)pp.title=s.last_error||'';
return s;}).catch(function(){pill('wifi','bad','Device offline');pill('mqtt','wait','MQTT -');pill('prn','wait','Printer -');return null;});}
window.refreshStatus=status;
(function loop(ms){setTimeout(function(){if(document.hidden)return loop(5000);status().then(function(s){loop(s&&s.job==='printing'?1500:5000);});},ms||5000);})();
function waitOnline(n){n=n||0;setTimeout(function(){status().then(function(s){
if(s&&s.wifi){toast('Device online','ok');setTimeout(function(){location.reload()},900);}
else if(n<40)waitOnline(n+1);
else toast('Device not reachable here. If Wi-Fi changed, open its new address or its .local name.','err');});},1500);}
var bar=$('.bar');function dirty(d){if(!bar)return;bar.classList.toggle('dirty',d);$('#dirtyT').textContent=d?'Unsaved changes':'No changes';}
document.querySelectorAll('form[data-ajax]').forEach(function(f){
f.addEventListener('input',function(){dirty(true)});f.addEventListener('change',function(){dirty(true)});
f.addEventListener('submit',function(e){e.preventDefault();var b=f.querySelector('button[type=submit]');
btn(b,'busy',f.dataset.busy||'Saving...');toast('Sending to device...','wait');
fetch(f.action,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(new FormData(f))})
.then(function(r){return r.text().then(function(t){if(!r.ok)throw new Error(t);
btn(b,'ok','Saved');dirty(false);
if(f.dataset.after==='wizard'){toast(t,'ok');}else{toast('Saved. Device is reconnecting...','wait');waitOnline();}});})
.catch(function(x){btn(b,'err','Failed');toast(String(x.message||'Request failed'),'err');});});});
document.querySelectorAll('form:not([data-ajax])').forEach(function(f){f.addEventListener('submit',function(){
var b=f.querySelector('button[type=submit]');if(b)setTimeout(function(){btn(b,'busy',f.dataset.busy||'Working...')},0);});});
document.querySelectorAll('[data-post]').forEach(function(b){b.addEventListener('click',function(){
btn(b,'busy',b.dataset.busy||'Working...');
fetch(b.dataset.post,{method:'POST'}).then(function(r){return r.text().then(function(t){if(!r.ok)throw new Error(t);btn(b,'ok',b.dataset.ok||'Done');toast(t,'ok');});})
.catch(function(x){btn(b,'err','Failed');toast(String(x.message||x),'err');});});});
addEventListener('pageshow',function(e){if(e.persisted)location.reload();});
)JS";

static const char kWifiJs[] = R"JS(
(function(){
var $i=function(i){return document.getElementById(i)};
var scan=$i('wifiScanBtn'),res=$i('wifiScanResults'),cb=$i('wifiConnectBtn'),ss=$i('wifiScanStatus'),cs=$i('wifiConnectStatus');
var ssid=document.querySelector("input[name='wifiSsid']"),pw=document.querySelector("input[name='wifiPassword']");
var st,ct,active=cb,H={'Content-Type':'application/x-www-form-urlencoded'};
function poll(){fetch('/wifi-scan/result').then(function(r){return r.json()}).then(function(d){
if(d.status==='scanning')return;clearInterval(st);
if(d.status==='done'&&d.networks&&d.networks.length){
ss.textContent='Pick a network from the list.';res.innerHTML='';
var ph=new Option('Select a network...','',true,true);ph.disabled=true;res.add(ph);
d.networks.forEach(function(n){res.add(new Option(n.ssid+' ('+n.rssi+' dBm'+(n.secure?'':', open')+')',n.ssid));});
res.hidden=false;btn(scan,'ok','Found '+d.networks.length);
}else if(d.status==='done'){ss.textContent='No networks found.';res.hidden=true;btn(scan,'err','None found');}
else{ss.textContent='Scan failed.';res.hidden=true;btn(scan,'err','Scan failed');}
}).catch(function(){clearInterval(st);btn(scan,'err','Scan failed');});}
scan.onclick=function(){btn(scan,'busy','Scanning...');ss.textContent='';res.hidden=true;cb.hidden=true;cs.textContent='';
fetch('/wifi-scan/start',{method:'POST'}).then(function(){clearInterval(st);st=setInterval(poll,800);}).catch(function(){btn(scan,'err','Failed');});};
res.onchange=function(){if(!res.value)return;ssid.value=res.value;cb.hidden=false;cs.textContent='';pw.focus();ssid.dispatchEvent(new Event('input',{bubbles:true}));};
function pc(label){fetch('/wifi-scan/connect-result').then(function(r){return r.json()}).then(function(d){
if(d.status==='connecting'){cs.textContent='Connecting to '+label+'...';return;}
clearInterval(ct);
if(d.status==='connected'){cs.textContent='Connected. IP '+d.ip;btn(active,'ok','Connected');toast('Connected to '+label,'ok');status();}
else{cs.textContent='Could not connect. Check the password and try again.';btn(active,'err','Failed');}
}).catch(function(){clearInterval(ct);btn(active,'err','Check failed');});}
function go(b,label,url,body){active=b;btn(b,'busy','Connecting...');cs.textContent='Connecting to '+label+'...';
fetch(url,{method:'POST',headers:H,body:body}).then(function(){clearInterval(ct);ct=setInterval(function(){pc(label)},1000);}).catch(function(){btn(b,'err','Failed');});}
cb.onclick=function(){if(ssid.value)go(cb,ssid.value,'/wifi-scan/connect','ssid='+encodeURIComponent(ssid.value)+'&password='+encodeURIComponent(pw.value));};
document.querySelectorAll('.savedWifiConnectBtn').forEach(function(b){b.onclick=function(){var s=b.getAttribute('data-ssid');go(b,s,'/wifi-scan/connect-saved','ssid='+encodeURIComponent(s));};});
document.querySelectorAll('.savedWifiForgetBtn').forEach(function(b){b.onclick=function(){btn(b,'busy','...');
fetch('/wifi-forget',{method:'POST',headers:H,body:'ssid='+encodeURIComponent(b.getAttribute('data-ssid'))}).then(function(){location.reload();}).catch(function(){btn(b,'err','Failed');});};});
})();
)JS";

static const char kFwJs[] = R"JS(
(function(){
var f=document.getElementById('fwFile'),b=document.getElementById('fwBtn'),p=document.getElementById('fwProg');
b.dataset.l='Upload & reboot';
f.onchange=function(){if(f.files.length){b.disabled=false;b.textContent='Upload & reboot';}else{b.disabled=true;b.textContent='Choose a .bin file';}};
document.getElementById('fw').onsubmit=function(e){e.preventDefault();if(!f.files.length)return;
var x=new XMLHttpRequest(),d=new FormData();d.append('firmware',f.files[0]);
p.hidden=false;p.value=0;f.disabled=true;btn(b,'busy','Uploading 0%');toast('Uploading firmware. Keep this page open.','wait');
x.open('POST','/update');
x.upload.onprogress=function(ev){if(ev.lengthComputable){var n=Math.round(ev.loaded*100/ev.total);p.value=n;btn(b,'busy','Uploading '+n+'%');}};
x.onload=function(){var t=x.responseText||'',m=/Update failed: ([^<]*)/.exec(t);
if(m||x.status!==200){p.hidden=true;f.disabled=false;btn(b,'err','Update failed');toast(m?m[1]:'Update failed','err');return;}
btn(b,'busy','Rebooting...');toast('Update applied. Waiting for the device to come back...','wait');setTimeout(function(){waitOnline();},3000);};
x.onerror=function(){p.hidden=true;f.disabled=false;btn(b,'err','Connection lost');toast('Connection lost during upload. Nothing was changed unless the device rebooted.','err');};
x.send(d);};
})();
)JS";

static const char kLogJs[] = R"JS(
(function(){
var since=0,el=$('#log'),as=$('#autoscroll'),ar=$('#autorefresh'),lvl=$('#lvl');
function fmt(ms){var s=Math.floor(ms/1000),p=function(n){return String(n).padStart(2,'0')};return '['+p(Math.floor(s/3600))+':'+p(Math.floor(s%3600/60))+':'+p(s%60)+']';}
async function poll(){
if(!ar.checked){pill('live','wait','Paused');setTimeout(poll,1500);return;}
try{var r=await fetch('/logs.json?since='+since,{cache:'no-store'}),d=await r.json();pill('live','ok','Live');
for(var e of d.entries){since=Math.max(since,e.seq);var l=document.createElement('div');l.className='lvl-'+e.level;l.textContent=fmt(e.ms)+' '+e.level+' '+e.msg;el.appendChild(l);}
while(el.childNodes.length>300)el.removeChild(el.firstChild);
if(d.entries.length&&as.checked)el.scrollTop=el.scrollHeight;
}catch(x){pill('live','bad','Offline');}
setTimeout(poll,1500);}
poll();
var H={'Content-Type':'application/x-www-form-urlencoded'};
lvl.onchange=function(){lvl.disabled=true;toast('Setting log level...','wait');
fetch('/logs/level',{method:'POST',headers:H,body:'level='+encodeURIComponent(lvl.value)}).then(function(){toast('Log level: '+lvl.value,'ok');}).catch(function(){toast('Could not set log level','err');}).then(function(){lvl.disabled=false;});};
var c=$('#clearBtn');c.onclick=function(){btn(c,'busy','Clearing...');
fetch('/logs/clear',{method:'POST'}).then(function(){el.textContent='';btn(c,'ok','Cleared');}).catch(function(){btn(c,'err','Failed');});};
})();
)JS";

// "ready" / "missing" for a USB-host printer, "n/a" when the transport has
// no way to tell (network/serial -- those only fail at send time).
static String printerState() {
  if (cfg.printerMethod == "usb_host") return usbHostTargetReady() ? "ready" : "missing";
  return "n/a";
}

static String pillHtml(const char* id, const char* cls, const String& text) {
  return String("<span class='pill ") + cls + "' id='p-" + id + "'><i></i><span>" + text + "</span></span>";
}

static String statusPillsHtml() {
  String h = "<div class='pills'>";
  if (wifiIsConnected()) h += pillHtml("wifi", "ok", "Wi-Fi " + WiFi.localIP().toString());
  else if (wifiApModeActive()) h += pillHtml("wifi", "wait", "Setup AP");
  else h += pillHtml("wifi", "bad", "Wi-Fi off");
  h += pillHtml("mqtt", mqttIsConnected() ? "ok" : "bad", mqttIsConnected() ? "MQTT on" : "MQTT off");
  const String ps = printerState();
  if (printQueueBusy()) h += pillHtml("prn", "wait", "Printing");
  else if (!printQueueLastError().isEmpty()) h += pillHtml("prn", "bad", "Last print failed");
  else if (ps == "ready") h += pillHtml("prn", "ok", "Printer ready");
  else if (ps == "missing") h += pillHtml("prn", "bad", "No printer");
  else h += pillHtml("prn", "wait", "Printer: " + String(targetMethodName()));
  h += "</div>";
  return h;
}

static String navLink(const char* href, const char* label, bool on) {
  return String("<a href='") + href + "'" + (on ? " class='on'" : "") + ">" + label + "</a>";
}

// active: "config" | "firmware" | "logs" | "" (no nav, e.g. setup wizard)
static String pageStart(const char* title, const char* active, bool nav) {
  String h;
  h.reserve(20000);
  h += "<!doctype html><html lang='en'><head><meta charset='utf-8'>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'><meta name='theme-color' content='#0f172a'>";
  h += "<title>Stikka - ";
  h += title;
  h += "</title><style>";
  h += kCss;
  h += "</style></head><body><header><div class='top'><b>Stikka bridge</b><span>";
  h += htmlEscape(cfg.printerName);
  h += "</span></div>";
  if (nav) {
    h += "<nav>";
    h += navLink("/", "Config", String(active) == "config");
    h += navLink("/firmware", "Firmware", String(active) == "firmware");
    h += navLink("/logs", "Logs", String(active) == "logs");
    h += "</nav>";
  }
  h += statusPillsHtml();
  h += "</header><main>";
  return h;
}

static void pageEnd(String& h, const char* extraJs) {
  h += "</main><div id='toast' role='status' aria-live='polite'></div><script>";
  h += kJs;
  h += "</script>";
  if (extraJs != nullptr) {
    h += "<script>";
    h += extraJs;
    h += "</script>";
  }
  h += "</body></html>";
}

static String cardOpen(const char* cls, const char* title) {
  return String("<section class='card ") + cls + "'><h2>" + title + "</h2>";
}

static String fieldHtml(const char* label, const char* name, const String& value, const char* type = "text", const char* extra = "") {
  return String("<label>") + label + "<input type='" + type + "' name='" + name + "' value='" + htmlEscape(value) + "' " + extra + "></label>";
}

static String checkHtml(const char* label, const char* name, bool checked) {
  return String("<label class='chk'><input type='checkbox' name='") + name + "'" + (checked ? " checked" : "") + "><span>" + label + "</span></label>";
}

static String saveBarHtml(const char* label) {
  return String("<div class='bar'><div><span class='dirty-t' id='dirtyT'>No changes</span><button type='submit' class='btn'>") + label + "</button></div></div>";
}

static String kvHtml(const char* k, const String& v) {
  return String("<div class='kv'><span>") + k + "</span><b>" + v + "</b></div>";
}

// The single <select> for choosing printer type (-> cfg.printerProtocol +
// cfg.printerMethod together), shared between renderWizardPage() (first
// boot) and the "Change printer type" box on renderConfigPage() (changing it
// later, see handleChangeProtocol()) -- both post the same 'printerType' field.
static String printerTypeFieldHtml() {
  size_t selectedIdx = 0;  // default to the first option (zpl/network) when unset or not one of the options above
  for (size_t i = 0; i < kPrinterTypeOptionCount; i++) {
    if (cfg.printerProtocol == kPrinterTypeOptions[i].protocol && cfg.printerMethod == kPrinterTypeOptions[i].method) {
      selectedIdx = i;
      break;
    }
  }
  String html;
  html += "<label>Printer type<select name='printerType'>";
  for (size_t i = 0; i < kPrinterTypeOptionCount; i++) {
    html += "<option value='" + String(kPrinterTypeOptions[i].value) + "'";
    if (i == selectedIdx) html += " selected";
    html += ">" + String(kPrinterTypeOptions[i].label) + "</option>";
  }
  html += "</select></label>";
  return html;
}

// Saved networks (up to MAX_SAVED_WIFI_NETWORKS, see rememberWifiNetwork()).
// Connect posts just the ssid to /wifi-scan/connect-saved (password is looked
// up server-side); Forget posts the ssid to /wifi-forget. Both are
// script-driven type='button's (see kWifiJs) -- NOT nested <form>s, which
// HTML forbids inside the page's big /save (or /setup) <form>: the browser
// closes the outer form at the first inner </form> and the real Save button
// ends up outside any form. And not submit buttons, so Enter in a text field
// can't trigger Forget.
static String savedWifiNetworksHtml() {
  if (cfg.savedWifiNetworkCount <= 0) return String();
  String html = "<h3>Saved networks</h3>";
  for (int i = 0; i < cfg.savedWifiNetworkCount; i++) {
    const String escaped = htmlEscape(cfg.savedWifiNetworks[i].ssid);
    html += "<div class='net'><span>" + escaped + "</span>";
    html += "<button type='button' class='btn sm savedWifiConnectBtn' data-ssid='" + escaped + "'>Connect</button>";
    html += "<button type='button' class='btn sm ghost red savedWifiForgetBtn' data-ssid='" + escaped + "'>Forget</button></div>";
  }
  return html;
}

// Wi-Fi card shared by wizard + config: SSID, password, scan/connect
// controls and saved list. kWifiJs finds the inputs by name.
static String wifiCardHtml(bool requireSsid) {
  String h = cardOpen("wifi", "Wi-Fi");
  h += fieldHtml("Network name (SSID)", "wifiSsid", cfg.wifiSsid, "text", requireSsid ? "autocapitalize='none' required" : "autocapitalize='none'");
  h += fieldHtml("Password", "wifiPassword", cfg.wifiPassword, "password", "autocomplete='off'");
  h += "<div class='row'><button type='button' id='wifiScanBtn' class='btn sm ghost'>Scan for networks</button></div>";
  h += "<select id='wifiScanResults' hidden></select>";
  h += "<div class='row'><button type='button' id='wifiConnectBtn' class='btn sm' hidden>Connect now</button></div>";
  h += "<small id='wifiScanStatus'></small><small id='wifiConnectStatus'></small>";
  h += savedWifiNetworksHtml();
  h += "</section>";
  return h;
}

static String mqttCardHtml(bool full) {
  String h = cardOpen("mqtt", "MQTT broker");
  h += fieldHtml("Broker host", "mqttHost", cfg.mqttHost);
  h += "<small>Host only (no ws:// or wss://), e.g. your-cluster.s2.eu.hivemq.cloud</small>";
  h += fieldHtml("Broker port", "mqttPort", String(cfg.mqttPort), "text", "inputmode='numeric'");
  h += checkHtml("Use TLS (HiveMQ Cloud: on, port 8883)", "mqttUseTls", cfg.mqttUseTls);
  h += checkHtml("TLS insecure mode (skip certificate validation)", "mqttTlsInsecure", cfg.mqttTlsInsecure);
  if (full) {
    h += "<label>CA certificate PEM (optional)<textarea name='mqttCaCert' rows='5'>" + htmlEscape(cfg.mqttCaCert) + "</textarea></label>";
  }
  h += fieldHtml("User", "mqttUser", cfg.mqttUser, "text", "autocapitalize='none' autocomplete='off'");
  h += fieldHtml("Password", "mqttPassword", cfg.mqttPassword, "password", "autocomplete='off'");
  if (full) {
    h += fieldHtml("Status publish interval (seconds)", "statusIntervalSec", String(cfg.statusIntervalSec), "text", "inputmode='numeric'");
  }
  h += "</section>";
  return h;
}

// Minimal first-boot form: Wi-Fi + MQTT + printer brand/method only. Shown
// instead of renderConfigPage() (see handleRoot()) until setupWizardComplete()
// is true -- once saved via POST /setup, later visits go straight to the
// full form.
static String renderWizardPage() {
  String html = pageStart("Setup", "", false);
  html += "<h1>Setup</h1>";
  html += "<p class='sub'>Step 1 of 2 &mdash; connect this bridge to your network and choose its printer. Label size and other details come next.</p>";
  html += "<form method='POST' action='/setup' data-ajax data-after='wizard' data-busy='Saving...'>";
  html += wifiCardHtml(true);
  html += mqttCardHtml(false);
  html += cardOpen("prn", "Printer");
  html += fieldHtml("Printer name", "printerName", cfg.printerName, "text", "autocapitalize='none'");
  html += "<small>Becomes the MQTT topic (/&lt;printer name&gt;/status/ and /command/), so it must be unique per bridge.</small>";
  html += printerTypeFieldHtml();
  html += "</section>";
  html += saveBarHtml("Save and continue");
  html += "</form>";
  pageEnd(html, kWifiJs);
  return html;
}

static String renderConfigPage() {
  String html = pageStart("Config", "config", true);
  html += "<h1>Bridge settings</h1>";
  html += "<p class='sub'>Wi-Fi, MQTT and printer settings. Changes apply when you press Save.</p>";

  // Device status (read-only snapshot at page load; the header pills stay live).
  html += cardOpen("dev", "Device");
  if (wifiIsConnected()) {
    String w = WiFi.localIP().toString();
    const String mdnsHostname = wifiMdnsHostname();
    if (!mdnsHostname.isEmpty()) w += " &middot; <a href='http://" + mdnsHostname + ".local/'>" + mdnsHostname + ".local</a>";
    html += kvHtml("Wi-Fi", w);
  } else {
    html += kvHtml("Wi-Fi", "disconnected");
  }
  html += kvHtml("MQTT", mqttIsConnected() ? "connected" : "disconnected");
  if (wifiApModeActive()) {
    html += kvHtml("Fallback AP", "<code>" + wifiFallbackApSsid() + "</code> / <code>" + String(wifiApPassword()) + "</code> &middot; " + WiFi.softAPIP().toString());
  } else {
    html += kvHtml("Fallback AP", "inactive");
  }
  html += kvHtml("Command topic", "<code>" + commandTopic() + "</code>");
  html += kvHtml("Status topic", "<code>" + statusTopic() + "</code>");
  html += "</section>";

  // Test print is an action, not a setting: its own button, own states.
  html += cardOpen("prn", "Test print");
  html += "<small style='margin:0'>Sends a small test label using the current saved settings.</small>";
  html += "<button type='button' class='btn teal block' data-post='/test' data-busy='Queueing...' data-ok='Queued'>Send test print</button>";
  html += "</section>";

  html += "<form method='POST' action='/save' data-ajax data-busy='Saving...'>";
  html += wifiCardHtml(false);

  html += cardOpen("prn", "Printer");
  html += fieldHtml("Printer name", "printerName", cfg.printerName, "text", "autocapitalize='none'");
  html += "<small>Type <code>" + cfg.printerProtocol + "</code> via <code>" + String(targetMethodName()) + "</code> (change it at the bottom of this page). Topics: /&lt;printer name&gt;/status/ and /command/.</small>";
  html += fieldHtml("Details (optional)", "location", cfg.location);
  html += "<small>Shown in the frontend next to this printer's serial number.</small>";
  if (cfg.printerMethod == "network") {
    html += "<h3>Network target</h3>";
    html += fieldHtml("Target host", "zplTargetHost", cfg.zplTargetHost);
    html += fieldHtml("Target port", "zplTargetPort", String(cfg.zplTargetPort), "text", "inputmode='numeric'");
  } else if (cfg.printerMethod == "serial") {
    html += "<h3>Serial target (UART)</h3><div class='cols'>";
    html += fieldHtml("TX pin", "printerUartTxPin", String(cfg.printerUartTxPin), "text", "inputmode='numeric'");
    html += fieldHtml("RX pin", "printerUartRxPin", String(cfg.printerUartRxPin), "text", "inputmode='numeric'");
    html += fieldHtml("Baud rate", "printerUartBaud", String(cfg.printerUartBaud), "text", "inputmode='numeric'");
    html += "</div><small>A dedicated hardware UART (not the USB/programming port) wired to the printer's serial input, 8N1.</small>";
  } else if (cfg.printerMethod == "usb_host") {
    html += "<h3>USB host target</h3><small>This board acts as a USB host and claims the printer plugged into its native USB port. Nothing to configure here.</small>";
  }
  html += "</section>";

  html += cardOpen("lbl", "Label");
  html += "<div class='cols'>";
  html += fieldHtml("DPI", "dpi", String(cfg.dpi), "text", "inputmode='numeric'");
  html += fieldHtml("Width mm", "labelWidth", String(cfg.labelWidth), "text", "inputmode='numeric'");
  html += fieldHtml("Length mm", "labelLength", String(cfg.labelLength), "text", "inputmode='numeric'");
  html += "</div><small>Length 0 = endless label (continuous roll, no fixed length).</small>";
  if (cfg.printerProtocol != "ql" && cfg.printerProtocol != "seiko") {
    html += checkHtml("Printer supports compressed graphics (:Z64:/:B64:)", "zplCompressionSupported", cfg.zplCompressionSupported);
    html += "<small>Not every ZPL printer implements this. Only enable if image labels print correctly with it on; if unsure, leave off.</small>";
  }
  html += "</section>";

  if (cfg.printerProtocol == "ql") {
    html += cardOpen("lbl", "Brother QL raster");
    html += fieldHtml("Printhead width px (720 = standard, 1296 = QL-11xx wide)", "qlPrintheadPx", String(cfg.qlPrintheadPx), "text", "inputmode='numeric'");
    html += fieldHtml("Invalidate bytes (200 = most models, 400 = QL-800/810W/820NWB)", "qlInvalidateBytes", String(cfg.qlInvalidateBytes), "text", "inputmode='numeric'");
    html += checkHtml("Auto-cut after each label", "qlAutoCut", cfg.qlAutoCut);
    html += "<div class='cols'>";
    html += fieldHtml("Feed margin (dots)", "qlFeedMarginDots", String(cfg.qlFeedMarginDots), "text", "inputmode='numeric'");
    html += fieldHtml("Right margin (dots, 0 = auto-center)", "qlRightMarginDots", String(cfg.qlRightMarginDots), "text", "inputmode='numeric'");
    html += "</div><small>Image jobs are resized to the label size above at 300dpi, thresholded to black/white and centered. No per-media offset table, dithering, two-color or status read-back. See stikka-esp32/README.md.</small>";
    html += "</section>";
  }

  if (cfg.printerProtocol == "seiko") {
    html += cardOpen("lbl", "Seiko SLP raster");
    html += fieldHtml("Max dots per line (576 = SLP-650)", "seikoMaxDots", String(cfg.seikoMaxDots), "text", "inputmode='numeric'");
    html += fieldHtml("Print density (0-255, 0 = 100%)", "seikoDensity", String(cfg.seikoDensity), "text", "inputmode='numeric'");
    html += fieldHtml("Print speed (0 = draft, 2 = fine/300dpi)", "seikoSpeed", String(cfg.seikoSpeed), "text", "inputmode='numeric'");
    html += "<small>Image jobs are resized to the label size above, thresholded to black/white and centered via the printer's margin command. See stikka-esp32/README.md.</small>";
    html += "</section>";
  }

  html += mqttCardHtml(true);

  html += cardOpen("dev", "Debug output");
  html += checkHtml("Enable serial debug output", "debugOutput", cfg.debugOutput);
  html += "<small>Always available on the programming/USB port (115200 8N1). The status LED's mode/pin/color are fixed by the firmware build.</small>";
  html += "</section>";

  html += saveBarHtml("Save and reconnect");
  html += "</form>";

  // Rare + disruptive (reboots), so it lives apart from the Save form, in red.
  html += "<form method='POST' action='/reconfigure' data-busy='Rebooting...'>";
  html += cardOpen("danger", "Change printer type");
  html += printerTypeFieldHtml();
  html += "<small>Reboots the device. Target host and QL/Seiko settings may need re-filling afterwards.</small>";
  html += "<button type='submit' class='btn red block'>Change &amp; reboot</button>";
  html += "</section></form>";

  pageEnd(html, kWifiJs);
  return html;
}

// OTA update over Wi-Fi (see handleOtaUpload()/handleOtaUpdate() below and
// Update.h) -- same firmware.bin scripts/build-firmware.sh stages for the
// in-browser USB flasher, just uploaded to the device directly instead of
// over a cable.
static String renderFirmwarePage() {
  String html = pageStart("Firmware", "firmware", true);
  html += "<h1>Firmware</h1>";
  html += "<p class='sub'>Update this bridge over Wi-Fi.</p>";
  html += cardOpen("dev", "Running build");
  html += kvHtml("Built", __DATE__ " " __TIME__);
  html += "</section>";

  html += cardOpen("lbl", "Upload new firmware");
  html += "<form id='fw'>";
  html += "<input type='file' id='fwFile' name='firmware' accept='.bin' required>";
  html += "<progress id='fwProg' max='100' value='0' hidden></progress>";
  html += "<button type='submit' id='fwBtn' class='btn amber block' disabled>Choose a .bin file</button>";
  html += "</form>";
  html += "<small>Upload the <code>firmware.bin</code> from <code>scripts/build-firmware.sh</code> (same file the browser USB flasher uses). The device reboots when done. Not password-protected: anyone who can reach this page can reflash the device.</small>";
  html += "</section>";
  pageEnd(html, kFwJs);
  return html;
}

static String renderLogsPage() {
  String html = pageStart("Logs", "logs", true);
  html += "<h1>Device logs</h1>";
  html += "<div class='tools' style='margin:.6rem 0 .8rem'>";
  html += "<label>Level <select id='lvl'>";
  static const char* kLevels[] = {"ERROR", "WARN", "INFO", "DEBUG"};
  for (const char* lvl : kLevels) {
    html += "<option value='" + String(lvl) + "'";
    if (String(logLevelName(cfg.logLevel)) == lvl) html += " selected";
    html += ">" + String(lvl) + "</option>";
  }
  html += "</select></label>";
  html += "<span class='pill lt wait' id='p-live' data-lt='1'><i></i><span>Connecting</span></span>";
  html += "<label><input type='checkbox' id='autoscroll' checked> Auto-scroll</label>";
  html += "<label><input type='checkbox' id='autorefresh' checked> Live</label>";
  html += "<button type='button' id='clearBtn' class='btn sm red'>Clear</button>";
  html += "</div>";
  html += "<div id='log'></div>";
  pageEnd(html, kLogJs);
  return html;
}

static void handleStatusJson() {
  String j = "{\"wifi\":";
  j += wifiIsConnected() ? "true" : "false";
  j += ",\"ip\":\"" + (wifiIsConnected() ? WiFi.localIP().toString() : String("")) + "\"";
  j += ",\"ap\":";
  j += wifiApModeActive() ? "true" : "false";
  j += ",\"mqtt\":";
  j += mqttIsConnected() ? "true" : "false";
  j += ",\"printer\":\"" + printerState() + "\"";
  j += ",\"method\":\"" + String(targetMethodName()) + "\"";
  j += ",\"job\":\"";
  j += printQueueBusy() ? "printing" : "idle";
  j += "\",\"pct\":" + String(printQueueProgressPct());
  j += ",\"queue\":" + String((unsigned long)printQueueWaiting());
  String le = printQueueLastError();
  le.replace("\\", "\\\\");
  le.replace("\"", "\\\"");
  j += ",\"last_error\":\"" + le + "\"}";
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", j);
}

static void handleRoot() {
  if (!setupWizardComplete()) {
    web.send(200, "text/html", renderWizardPage());
    return;
  }
  web.send(200, "text/html", renderConfigPage());
}

static void handleFirmwarePage() {
  web.send(200, "text/html", renderFirmwarePage());
}

static void handleCaptivePortal() {
  web.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  web.sendHeader("Pragma", "no-cache");
  web.sendHeader("Expires", "-1");
  handleRoot();
}

static void handleCaptivePortalRedirect() {
  if (wifiApModeActive()) {
    web.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
    web.send(302, "text/plain", "");
    return;
  }
  web.send(404, "text/plain", "not found");
}

// POST /setup -- the first-boot wizard's only handler. Deliberately separate
// from handleSave() rather than one "smart" handler for both forms: Arduino
// WebServer's web.hasArg("checkbox") is indistinguishable between "unchecked"
// and "field doesn't exist on this page", so a shared handler trying to
// leave fields untouched when absent would silently reset every checkbox
// (mqttUseTls, zplCompressionSupported, ...) the first time it's reached
// from a page that doesn't render them yet. Two handlers, each unconditionally
// parsing only its own page's known fields, avoids that trap.
static void handleWizardSave() {
  cfg.wifiSsid = web.arg("wifiSsid");
  cfg.wifiPassword = web.arg("wifiPassword");
  rememberWifiNetwork(cfg.wifiSsid, cfg.wifiPassword);
  cfg.mqttHost = normalizeMqttHost(web.arg("mqttHost"));
  cfg.mqttPort = parsePort(web.arg("mqttPort"), 1883);
  cfg.mqttUseTls = web.hasArg("mqttUseTls");
  cfg.mqttTlsInsecure = web.hasArg("mqttTlsInsecure");
  cfg.mqttUser = web.arg("mqttUser");
  cfg.mqttPassword = web.arg("mqttPassword");

  cfg.printerName = web.arg("printerName");
  if (cfg.printerName.isEmpty()) cfg.printerName = "stikka-esp32";

  String printerType = web.arg("printerType");
  printerType.toLowerCase();
  if (!printerTypeFromValue(printerType, cfg.printerProtocol, cfg.printerMethod)) {
    cfg.printerProtocol = "zpl";
    cfg.printerMethod = "network";
  }

  // Pre-fill DPI with a protocol-appropriate default the moment brand is
  // first chosen, rather than leaving the generic fallback in place --
  // Brother QL and Seiko SLP printers are commonly 300dpi vs. 203dpi for
  // typical ZPL printers. Only applies while dpi is still untouched from its
  // default.
  if (cfg.dpi == DEFAULT_DPI) {
    cfg.dpi = (cfg.printerProtocol == "ql" || cfg.printerProtocol == "seiko") ? 300 : DEFAULT_DPI;
  }

  saveConfig();
  applyDebugOutputSetting(cfg.debugOutput);
  targetSetup();
  dbgPrintln("[config] setup wizard step 1 saved, reconnecting wifi/mqtt", LogLevel::LOG_INFO);
  printRuntimeSettings("setup wizard step 1 saved");

  // See handleSave()'s comment below on why the response is sent before the
  // network is torn down.
  web.send(200, "text/plain", "saved; reconnecting wifi and mqtt -- once back on your normal network, browse to this device again to finish setup");
  web.client().flush();
  delay(250);

  wifiForceDisconnect();
  wifiDisableFallbackAp();
}

// POST /reconfigure -- the "Printer type" box's handler, letting an
// already-set-up device switch brand/method later without redoing the whole
// wizard (and, unlike handleWizardSave(), without touching Wi-Fi/MQTT at
// all). Always reboots on an actual change rather than live re-initializing
// the target in place: target.cpp's dispatcher and each *_target.cpp were
// written assuming targetSetup() runs exactly once per boot (usb_host_target.cpp
// in particular installs the USB host stack with no teardown path -- calling
// usbHostTargetSetup() a second time this boot, e.g. after switching away
// from and back to usb_host, would double-install it). A fresh boot sidesteps
// that entirely instead of adding per-target teardown just for this button.
static void handleChangeProtocol() {
  String printerType = web.arg("printerType");
  printerType.toLowerCase();
  String protocol = cfg.printerProtocol;
  String method = cfg.printerMethod;
  printerTypeFromValue(printerType, protocol, method);

  const bool changed = (protocol != cfg.printerProtocol) || (method != cfg.printerMethod);
  const bool protocolChanged = (protocol != cfg.printerProtocol);
  cfg.printerProtocol = protocol;
  cfg.printerMethod = method;

  // Same DPI nicety as the initial wizard save -- only nudges it if still
  // untouched from the generic default.
  if (protocolChanged && cfg.dpi == DEFAULT_DPI) {
    cfg.dpi = (cfg.printerProtocol == "ql" || cfg.printerProtocol == "seiko") ? 300 : DEFAULT_DPI;
  }

  saveConfig();

  if (!changed) {
    web.sendHeader("Location", "/", true);
    web.send(302, "text/plain", "");
    return;
  }

  dbgPrintln("[config] printer type changed via web UI, rebooting", LogLevel::LOG_INFO);
  web.send(200, "text/html",
      "<!doctype html><meta http-equiv='refresh' content='4;url=/'><body>Saved. Rebooting&hellip; this page will reload shortly.</body>");
  web.client().flush();
  delay(250);
  ESP.restart();
}

// POST /update -- the "Firmware" box's file upload (see renderConfigPage()).
// Standard Arduino-ESP32 Update.h flow: WebServer streams the multipart
// upload to this pair of handlers chunk-by-chunk via HTTPUpload, one status
// transition at a time, rather than buffering the whole ~1MB file in RAM
// first. otaError latches on the first failure so a bad UPLOAD_FILE_WRITE
// doesn't get masked by a later status transition still reporting success.
static bool otaSucceeded = false;
static String otaError;

static void handleOtaUpload() {
  HTTPUpload& upload = web.upload();
  if (upload.status == UPLOAD_FILE_START) {
    otaSucceeded = false;
    otaError = "";
    dbgPrint("[ota] update started: ");
    dbgPrintln(upload.filename, LogLevel::LOG_ERROR);
    if (!Update.begin()) {
      StreamString err;
      Update.printError(err);
      otaError = err;
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!otaError.isEmpty()) return;
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      StreamString err;
      Update.printError(err);
      otaError = err;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (otaError.isEmpty()) {
      otaSucceeded = Update.end(/*evenIfRemaining=*/true);
      if (!otaSucceeded) {
        StreamString err;
        Update.printError(err);
        otaError = err;
      }
    }
    if (otaSucceeded) {
      dbgPrint("[ota] update succeeded, ");
      dbgPrint(upload.totalSize);
      dbgPrintln(" bytes written -- rebooting", LogLevel::LOG_ERROR);
    } else {
      dbgPrint("[ota] update FAILED: ");
      dbgPrintln(otaError, LogLevel::LOG_ERROR);
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaError = "upload aborted (connection dropped?)";
    dbgPrintln("[ota] update aborted, connection dropped", LogLevel::LOG_ERROR);
  }
}

static void handleOtaUpdate() {
  if (otaSucceeded) {
    web.send(200, "text/html",
        "<!doctype html><meta http-equiv='refresh' content='4;url=/firmware'><body>Update applied. Rebooting&hellip; this page will reload shortly.</body>");
    web.client().flush();
    delay(250);
    ESP.restart();
    return;
  }
  web.send(200, "text/html",
      "<!doctype html><body>Update failed: " + htmlEscape(otaError.isEmpty() ? String("no file received") : otaError) +
      "<br>Nothing was changed -- the device is still running its current firmware.<br><a href='/firmware'>Back</a></body>");
}

static void handleSave() {
  cfg.wifiSsid = web.arg("wifiSsid");
  cfg.wifiPassword = web.arg("wifiPassword");
  rememberWifiNetwork(cfg.wifiSsid, cfg.wifiPassword);
  cfg.mqttHost = normalizeMqttHost(web.arg("mqttHost"));
  cfg.mqttPort = parsePort(web.arg("mqttPort"), 1883);
  cfg.mqttUseTls = web.hasArg("mqttUseTls");
  cfg.mqttTlsInsecure = web.hasArg("mqttTlsInsecure");
  cfg.mqttCaCert = web.arg("mqttCaCert");
  cfg.mqttUser = web.arg("mqttUser");
  cfg.mqttPassword = web.arg("mqttPassword");
  cfg.statusIntervalSec = (uint16_t)parsePort(web.arg("statusIntervalSec"), 30);
  if (cfg.statusIntervalSec < 1) cfg.statusIntervalSec = 1;
  if (cfg.statusIntervalSec > 3600) cfg.statusIntervalSec = 3600;
  cfg.printerName = web.arg("printerName");
  cfg.location = web.arg("location");
  if (cfg.printerMethod == "network") {
    cfg.zplTargetHost = web.arg("zplTargetHost");
    cfg.zplTargetPort = parsePort(web.arg("zplTargetPort"), 9100);
  } else if (cfg.printerMethod == "serial") {
    cfg.printerUartTxPin = web.arg("printerUartTxPin").toInt();
    cfg.printerUartRxPin = web.arg("printerUartRxPin").toInt();
    cfg.printerUartBaud = (uint32_t)web.arg("printerUartBaud").toInt();
    if (cfg.printerUartBaud == 0) cfg.printerUartBaud = 9600;
  }
  // usb_host: nothing to save -- the printer is auto-enumerated, no fields posted.
  cfg.dpi = web.arg("dpi").toInt();
  if (cfg.dpi <= 0) cfg.dpi = DEFAULT_DPI;
  cfg.labelWidth = web.arg("labelWidth").toInt();
  if (cfg.labelWidth <= 0) cfg.labelWidth = 55;
  cfg.labelLength = web.arg("labelLength").toInt();
  // 0 is a valid, deliberate value here: it means "endless label" (continuous
  // roll, no fixed length) throughout the frontend (zpl-image.ts, mqtt-api.ts,
  // editor.ts, ui.ts). Only negative input is nonsense and falls back to the default.
  if (cfg.labelLength < 0) cfg.labelLength = 55;
  if (cfg.printerProtocol != "ql" && cfg.printerProtocol != "seiko") {
    cfg.zplCompressionSupported = web.hasArg("zplCompressionSupported");
  }

  if (cfg.printerProtocol == "ql") {
    cfg.qlPrintheadPx = web.arg("qlPrintheadPx").toInt() == 1296 ? 1296 : 720;
    cfg.qlInvalidateBytes = web.arg("qlInvalidateBytes").toInt();
    if (cfg.qlInvalidateBytes <= 0) cfg.qlInvalidateBytes = 200;
    cfg.qlAutoCut = web.hasArg("qlAutoCut");
    cfg.qlFeedMarginDots = web.arg("qlFeedMarginDots").toInt();
    if (cfg.qlFeedMarginDots < 0) cfg.qlFeedMarginDots = 35;
    cfg.qlRightMarginDots = web.arg("qlRightMarginDots").toInt();
    if (cfg.qlRightMarginDots < 0) cfg.qlRightMarginDots = 0;
  }

  if (cfg.printerProtocol == "seiko") {
    cfg.seikoMaxDots = web.arg("seikoMaxDots").toInt();
    if (cfg.seikoMaxDots <= 0) cfg.seikoMaxDots = 576;
    cfg.seikoDensity = web.arg("seikoDensity").toInt();
    if (cfg.seikoDensity < 0) cfg.seikoDensity = 0;
    if (cfg.seikoDensity > 255) cfg.seikoDensity = 255;
    cfg.seikoSpeed = web.arg("seikoSpeed").toInt();
    if (cfg.seikoSpeed < 0) cfg.seikoSpeed = 0;
    if (cfg.seikoSpeed > 255) cfg.seikoSpeed = 255;
  }

  cfg.debugOutput = web.hasArg("debugOutput");

  if (cfg.printerName.isEmpty()) cfg.printerName = "stikka-esp32";

  saveConfig();
  applyDebugOutputSetting(cfg.debugOutput);
  targetSetup();
  dbgPrintln("[config] settings saved via web UI, reconnecting wifi/mqtt", LogLevel::LOG_INFO);
  printRuntimeSettings("config saved from web UI");

  // Send the response before tearing down the network. The browser's request
  // often arrived over the ESP32's own fallback AP -- if wifiDisableFallbackAp()
  // (WiFi.softAPdisconnect) runs first, that AP link drops before the HTTP
  // response is ever written to the socket, and the browser hangs forever
  // waiting for a reply that can no longer arrive. The delay after send()
  // gives the WiFi stack a moment to actually get the response bytes over the
  // air before the interface goes down.
  web.send(200, "text/plain", "saved; reconnecting wifi and mqtt");
  web.client().flush();
  delay(250);

  wifiForceDisconnect();
  wifiDisableFallbackAp();
}

// POST /test -- queues a firmware-built test label through the same print
// queue as MQTT jobs, so it can't collide with a job already being sent and
// never blocks the web server. Progress shows in the Printer status pill.
static void handleTest() {
  std::unique_ptr<uint8_t[]> bytes;
  size_t len = 0;
  if (cfg.printerProtocol == "ql" || cfg.printerProtocol == "seiko") {
    // No client-supplied image exists for a firmware-triggered test print,
    // so this builds a small synthetic striped raster (see test_print.cpp).
    const bool built = (cfg.printerProtocol == "ql")
        ? buildQlTestPrintBytes(bytes, len)
        : buildSeikoTestPrintBytes(bytes, len);
    if (!built) {
      web.send(500, "text/plain", "test failed: could not build test pattern (out of memory)");
      return;
    }
  } else {
    static const char kZpl[] = "^XA^CF0,30^FO40,40^FDStikka ESP32 test^FS^XZ";
    len = sizeof(kZpl) - 1;
    bytes.reset(new (std::nothrow) uint8_t[len]);
    if (!bytes) {
      web.send(500, "text/plain", "test failed: out of memory");
      return;
    }
    memcpy(bytes.get(), kZpl, len);
  }
  String err;
  if (!printQueueEnqueue("web-test-" + String(millis()), "test", std::move(bytes), len, err)) {
    dbgPrintln("[test] not queued: " + err, LogLevel::LOG_WARN);
    web.send(503, "text/plain", err);
    return;
  }
  dbgPrintln("[test] test label queued", LogLevel::LOG_INFO);
  web.send(200, "text/plain", "Test label queued. The Printer pill shows progress.");
}

static void handleLogsPage() {
  web.send(200, "text/html", renderLogsPage());
}

static void handleLogsJson() {
  uint32_t since = 0;
  if (web.hasArg("since")) since = (uint32_t)strtoul(web.arg("since").c_str(), nullptr, 10);
  web.send(200, "application/json", logEntriesToJson(since));
}

static void handleLogsSetLevel() {
  cfg.logLevel = logLevelFromString(web.arg("level"), cfg.logLevel);
  saveConfig();
  dbgPrintln(String("[config] log level set to ") + logLevelName(cfg.logLevel), LogLevel::LOG_INFO);
  web.sendHeader("Location", "/logs", true);
  web.send(302, "text/plain", "");
}

static void handleLogsClear() {
  logBufferClear();
  web.sendHeader("Location", "/logs", true);
  web.send(302, "text/plain", "");
}

static void handleWifiScanStart() {
  wifiStartScan();
  web.send(202, "text/plain", "scanning");
}

static void handleWifiScanResult() {
  web.send(200, "application/json", wifiScanResultJson());
}

static void handleWifiScanConnect() {
  String ssid = web.arg("ssid");
  if (ssid.isEmpty()) {
    web.send(400, "text/plain", "ssid required");
    return;
  }
  wifiConnectTo(ssid, web.arg("password"));
  web.send(202, "text/plain", "connecting");
}

static void handleWifiConnectResult() {
  web.send(200, "application/json", wifiConnectStatusJson());
}

// POST /wifi-scan/connect-saved -- the "Connect" button next to an entry in
// the "Saved networks" list (see savedWifiNetworksHtml()). Takes only ssid;
// the password is looked up server-side from cfg.savedWifiNetworks rather
// than round-tripped through the browser, so a saved network's password
// never has to appear in the page again once it's been entered once.
static void handleWifiScanConnectSaved() {
  String ssid = web.arg("ssid");
  for (int i = 0; i < cfg.savedWifiNetworkCount; i++) {
    if (cfg.savedWifiNetworks[i].ssid == ssid) {
      wifiConnectTo(ssid, cfg.savedWifiNetworks[i].password);
      web.send(202, "text/plain", "connecting");
      return;
    }
  }
  web.send(404, "text/plain", "not a saved network");
}

// POST /wifi-forget -- removes one entry from the "Saved networks" list.
// Plain form POST + redirect (like /reconfigure's no-change branch and the
// /logs/clear button) rather than fetch()+JS, since this is a rare,
// low-frequency action that doesn't need live progress feedback the way
// connecting does.
static void handleWifiForget() {
  forgetWifiNetwork(web.arg("ssid"));
  web.sendHeader("Location", "/", true);
  web.send(302, "text/plain", "");
}

void webUiSetup() {
  web.on("/", HTTP_GET, handleRoot);
  web.on("/generate_204", HTTP_GET, handleCaptivePortalRedirect);
  web.on("/gen_204", HTTP_GET, handleCaptivePortalRedirect);
  web.on("/hotspot-detect.html", HTTP_GET, handleCaptivePortal);
  web.on("/connecttest.txt", HTTP_GET, handleCaptivePortal);
  web.on("/ncsi.txt", HTTP_GET, handleCaptivePortal);
  web.on("/fwlink", HTTP_GET, handleCaptivePortalRedirect);
  web.on("/setup", HTTP_POST, handleWizardSave);
  web.on("/save", HTTP_POST, handleSave);
  web.on("/reconfigure", HTTP_POST, handleChangeProtocol);
  web.on("/firmware", HTTP_GET, handleFirmwarePage);
  web.on("/status.json", HTTP_GET, handleStatusJson);
  web.on("/update", HTTP_POST, handleOtaUpdate, handleOtaUpload);
  web.on("/wifi-scan/start", HTTP_POST, handleWifiScanStart);
  web.on("/wifi-scan/result", HTTP_GET, handleWifiScanResult);
  web.on("/wifi-scan/connect", HTTP_POST, handleWifiScanConnect);
  web.on("/wifi-scan/connect-result", HTTP_GET, handleWifiConnectResult);
  web.on("/wifi-scan/connect-saved", HTTP_POST, handleWifiScanConnectSaved);
  web.on("/wifi-forget", HTTP_POST, handleWifiForget);
  web.on("/logs", HTTP_GET, handleLogsPage);
  web.on("/logs.json", HTTP_GET, handleLogsJson);
  web.on("/logs/level", HTTP_POST, handleLogsSetLevel);
  web.on("/logs/clear", HTTP_POST, handleLogsClear);
  web.on("/test", HTTP_POST, handleTest);
  web.onNotFound(handleCaptivePortalRedirect);
  web.begin();
}

void webUiLoop() {
  web.handleClient();
}
