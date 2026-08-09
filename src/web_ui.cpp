#include "web_ui.h"

#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "logging.h"
#include "mqtt_bridge.h"
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
// firmware itself can run other combinations (serial method, Seiko SLP
// protocol -- see targets/serial_target.cpp, the "seiko" branches throughout
// this file), but they haven't been validated against real hardware yet, so
// the web UI deliberately doesn't offer them. Add a row here once a new
// combination has been confirmed working.
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

// The single <select> for choosing printer type (-> cfg.printerProtocol +
// cfg.printerMethod together), shared between renderWizardPage() (first
// boot) and the "Printer type" box on renderConfigPage() (changing it later,
// see handleChangeProtocol()) -- both post the same 'printerType' field.
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

// Scan button + status line + results dropdown, dropped in next to the SSID
// field on both renderWizardPage() and renderConfigPage(). Targets the
// nearest input[name='wifiSsid'] on the page rather than an id, since both
// pages already use that same field name and this markup is identical on
// both -- see wifiScanScriptTag() below for the polling logic.
static String wifiScanWidgetHtml() {
  String html;
  html += "<button type='button' id='wifiScanBtn' style='width:auto;margin-top:.5rem;'>Scan for networks</button>";
  html += "<div id='wifiScanStatus' style='margin-top:.35rem;font-size:.85rem;color:#666;'></div>";
  html += "<select id='wifiScanResults' style='display:none;'></select>";
  return html;
}

// POST /wifi-scan/start kicks off an async scan; this polls GET
// /wifi-scan/result (wifiScanResultJson() in wifi_manager.cpp) every 800ms
// until it settles, then fills #wifiScanResults. Picking an option copies
// its SSID into the nearest wifiSsid text input -- it doesn't auto-submit,
// so password entry (and, on renderConfigPage(), everything else on the
// form) is unaffected.
static String wifiScanScriptTag() {
  String html;
  html += "<script>(function(){";
  html += "var btn=document.getElementById('wifiScanBtn');";
  html += "var status=document.getElementById('wifiScanStatus');";
  html += "var sel=document.getElementById('wifiScanResults');";
  html += "var ssidInput=document.querySelector(\"input[name='wifiSsid']\");";
  html += "var timer=null;";
  html += "function poll(){";
  html += "fetch('/wifi-scan/result').then(function(r){return r.json();}).then(function(data){";
  html += "if(data.status==='scanning'){status.textContent='Scanning...';return;}";
  html += "clearInterval(timer);btn.disabled=false;";
  html += "if(data.status==='done'&&data.networks&&data.networks.length){";
  html += "status.textContent='Found '+data.networks.length+' network(s).';";
  html += "sel.innerHTML='';";
  html += "data.networks.forEach(function(net){";
  html += "var opt=document.createElement('option');";
  html += "opt.value=net.ssid;";
  html += "opt.textContent=net.ssid+' ('+net.rssi+' dBm'+(net.secure?'':', open')+')';";
  html += "sel.appendChild(opt);});";
  html += "sel.style.display='';";
  html += "}else if(data.status==='done'){status.textContent='No networks found.';sel.style.display='none';}";
  html += "else{status.textContent='Scan failed.';sel.style.display='none';}";
  html += "}).catch(function(){clearInterval(timer);btn.disabled=false;status.textContent='Scan failed.';});";
  html += "}";
  html += "btn.addEventListener('click',function(){";
  html += "btn.disabled=true;status.textContent='Scanning...';sel.style.display='none';";
  html += "fetch('/wifi-scan/start',{method:'POST'}).then(function(){timer=setInterval(poll,800);});";
  html += "});";
  html += "sel.addEventListener('change',function(){if(sel.value)ssidInput.value=sel.value;});";
  html += "})();</script>";
  return html;
}

// Minimal first-boot form: Wi-Fi + MQTT + printer brand/method only. Shown
// instead of renderConfigPage() (see handleRoot()) until setupWizardComplete()
// is true -- once saved via POST /setup, later visits go straight to the
// full form, which now shows brand/method-specific fields at runtime instead
// of gating them at compile time.
static String renderWizardPage() {
  String html;
  html.reserve(3000);
  html += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Stikka ESP32 - Setup</title><style>";
  html += "body{font-family:Arial,sans-serif;max-width:480px;margin:1rem auto;padding:0 1rem;}";
  html += "h1{margin-bottom:.25rem;} .sub{color:#555;margin-top:0;margin-bottom:1rem;}";
  html += "label{display:block;font-weight:600;margin-top:.75rem;} input,select{width:100%;padding:.5rem;margin-top:.25rem;}";
  html += "button{margin-top:1.5rem;padding:.6rem 1rem;width:100%;} .box{border:1px solid #ddd;border-radius:8px;padding:1rem;margin-bottom:1rem;}";
  html += "small{color:#666;}";
  html += "</style></head><body>";
  html += "<h1>Stikka ESP32 Setup</h1>";
  html += "<p class='sub'>Step 1 of 2 &mdash; connect this bridge to your network and tell it what kind of printer it's driving. Label size, target address and other details come next.</p>";

  html += "<form method='POST' action='/setup'>";
  html += "<div class='box'><h3>Wi-Fi</h3>";
  html += "<label>SSID<input name='wifiSsid' value='" + htmlEscape(cfg.wifiSsid) + "' required></label>";
  html += wifiScanWidgetHtml();
  html += "<label>Password<input type='password' name='wifiPassword' value='" + htmlEscape(cfg.wifiPassword) + "'></label>";
  html += "</div>";

  html += "<div class='box'><h3>MQTT broker</h3>";
  html += "<label>Broker host<input name='mqttHost' value='" + htmlEscape(cfg.mqttHost) + "'></label>";
  html += "<label>Broker port<input name='mqttPort' value='" + String(cfg.mqttPort) + "'></label>";
  html += "<label><input type='checkbox' name='mqttUseTls' ";
  if (cfg.mqttUseTls) html += "checked";
  html += "> Use TLS (HiveMQ Cloud: on, port 8883)</label>";
  html += "<label><input type='checkbox' name='mqttTlsInsecure' ";
  if (cfg.mqttTlsInsecure) html += "checked";
  html += "> TLS insecure mode (skip certificate validation)</label>";
  html += "<label>User<input name='mqttUser' value='" + htmlEscape(cfg.mqttUser) + "'></label>";
  html += "<label>Password<input type='password' name='mqttPassword' value='" + htmlEscape(cfg.mqttPassword) + "'></label>";
  html += "<small>Host only (no ws:// or wss://). Example: your-cluster.s2.eu.hivemq.cloud</small>";
  html += "</div>";

  html += "<div class='box'><h3>Printer</h3>";
  html += "<label>Printer name<input name='printerName' value='" + htmlEscape(cfg.printerName) + "'></label>";
  html += "<small>Becomes the MQTT topic (/&lt;printer name&gt;/status/, /&lt;printer name&gt;/command/), so it must be unique per bridge.</small>";
  html += printerTypeFieldHtml();
  html += "</div>";

  html += "<button type='submit'>Save and continue</button>";
  html += "</form>";
  html += wifiScanScriptTag();
  html += "</body></html>";
  return html;
}

static String renderConfigPage() {
  String html;
  html.reserve(9000);
  html += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Stikka ESP32</title><style>";
  html += "body{font-family:Arial,sans-serif;max-width:1400px;margin:1rem auto;padding:0 1rem;}";
  html += "h1{margin-bottom:.25rem;} .sub{color:#555;margin-top:0;margin-bottom:1rem;}";
  html += "label{display:block;font-weight:600;margin-top:.75rem;} input{width:100%;padding:.5rem;margin-top:.25rem;}";
  html += "button{margin-top:1rem;padding:.6rem 1rem;} .grid{column-width:320px;column-gap:1rem;}";
  html += ".box{border:1px solid #ddd;border-radius:8px;padding:1rem;margin-bottom:1rem;break-inside:avoid;}";
  html += "small{color:#666;} code{background:#f3f3f3;padding:.1rem .3rem;border-radius:3px;}";
  html += "nav{margin-bottom:1rem;} nav a{margin-right:1rem;font-weight:600;text-decoration:none;color:#333;}";
  html += "</style></head><body>";
  html += "<nav><a href='/'>Config</a><a href='/logs'>Logs</a></nav>";
  html += "<h1>Stikka ESP32 Bridge</h1>";
  html += "<p class='sub'>Configure Wi-Fi, MQTT and ZPL target for this printer bridge.</p>";

  html += "<div class='box'><strong>Runtime status</strong><br>";
  html += "Wi-Fi: ";
  html += wifiIsConnected() ? "connected" : "disconnected";
  if (wifiIsConnected()) {
    html += " (" + WiFi.localIP().toString() + ")";
    const String mdnsHostname = wifiMdnsHostname();
    if (!mdnsHostname.isEmpty()) {
      html += " -- <a href='http://" + mdnsHostname + ".local/'>" + mdnsHostname + ".local</a>";
    }
  }
  html += "<br>MQTT: ";
  html += mqttIsConnected() ? "connected" : "disconnected";
  html += "<br>Fallback AP: ";
  if (wifiApModeActive()) {
    html += "active (SSID: <code>" + wifiFallbackApSsid() + "</code>, password: <code>" + String(wifiApPassword()) + "</code>)";
    html += "<br>AP IP: " + WiFi.softAPIP().toString();
  } else {
    html += "inactive";
  }
  html += "<br>Command topic: <code>" + commandTopic() + "</code>";
  html += "<br>Status topic: <code>" + statusTopic() + "</code>";
  html += "</div>";

  // Deliberately its own <form>/box, outside the /save form above rather
  // than folded into the "Wi-Fi & Printer Name" box -- changing brand/method
  // reboots the device (see handleChangeProtocol()), which is a bigger/rarer
  // action than a normal settings save and shouldn't be triggerable by
  // accident from the main Save button.
  html += "<div class='box'><h3>Printer type</h3>";
  html += "<form method='POST' action='/reconfigure'>";
  html += printerTypeFieldHtml();
  html += "<button type='submit'>Change &amp; reboot</button>";
  html += "<small>Changing this reboots the device. Method/protocol-specific fields above (target host, QL settings) may need re-filling afterward.</small>";
  html += "</form>";
  html += "</div>";


  html += "<form method='POST' action='/save'>";
  html += "<div class='grid'>";
  html += "<div class='box'><h3>Wi-Fi &amp; Printer Name</h3>";
  html += "<label>SSID<input name='wifiSsid' value='" + htmlEscape(cfg.wifiSsid) + "'></label>";
  html += wifiScanWidgetHtml();
  html += "<label>Password<input type='password' name='wifiPassword' value='" + htmlEscape(cfg.wifiPassword) + "'></label>";
  html += "<label>Printer name<input name='printerName' value='" + htmlEscape(cfg.printerName) + "'></label>";
  html += "<small>Type: <code>" + cfg.printerProtocol + "</code> · transport: <code>";
  html += targetMethodName();
  html += "</code> (see \"Printer type\" below to change) · status topic: /&lt;printername&gt;/status/ · command topic: /&lt;printername&gt;/command/</small>";
  html += "<label>Details (optional)<input name='location' value='" + htmlEscape(cfg.location) + "'></label>";
  html += "<small>Shown in the frontend next to this printer's serial number.</small>";
  html += "</div>";

  html += "<div class='box'><h3>MQTT</h3>";
  html += "<label>Broker host<input name='mqttHost' value='" + htmlEscape(cfg.mqttHost) + "'></label>";
  html += "<label>Broker port<input name='mqttPort' value='" + String(cfg.mqttPort) + "'></label>";
  html += "<label><input type='checkbox' name='mqttUseTls' ";
  if (cfg.mqttUseTls) html += "checked";
  html += "> Use TLS (HiveMQ Cloud: on, port 8883)</label>";
  html += "<label><input type='checkbox' name='mqttTlsInsecure' ";
  if (cfg.mqttTlsInsecure) html += "checked";
  html += "> TLS insecure mode (skip certificate validation)</label>";
  html += "<label>CA certificate PEM (optional)<textarea name='mqttCaCert' rows='6' style='width:100%;margin-top:.25rem;'>" + htmlEscape(cfg.mqttCaCert) + "</textarea></label>";
  html += "<label>User<input name='mqttUser' value='" + htmlEscape(cfg.mqttUser) + "'></label>";
  html += "<label>Password<input type='password' name='mqttPassword' value='" + htmlEscape(cfg.mqttPassword) + "'></label>";
  html += "<label>Status publish interval (seconds)<input name='statusIntervalSec' value='" + String(cfg.statusIntervalSec) + "'></label>";
  html += "<small>Host only (no ws:// or wss://). Example: your-cluster.s2.eu.hivemq.cloud</small>";
  html += "</div>";

  if (cfg.printerMethod == "network") {
    html += "<div class='box'><h3>Network target</h3>";
    html += "<label>Target host<input name='zplTargetHost' value='" + htmlEscape(cfg.zplTargetHost) + "'></label>";
    html += "<label>Target port<input name='zplTargetPort' value='" + String(cfg.zplTargetPort) + "'></label>";
    html += "</div>";
  } else if (cfg.printerMethod == "serial") {
    html += "<div class='box'><h3>Serial target (UART)</h3>";
    html += "<label>TX pin<input name='printerUartTxPin' value='" + String(cfg.printerUartTxPin) + "'></label>";
    html += "<label>RX pin<input name='printerUartRxPin' value='" + String(cfg.printerUartRxPin) + "'></label>";
    html += "<label>Baud rate<input name='printerUartBaud' value='" + String(cfg.printerUartBaud) + "'></label>";
    html += "<small>A dedicated hardware UART (not the USB/programming port) wired directly to the printer's serial input, 8N1.</small>";
    html += "</div>";
  } else if (cfg.printerMethod == "usb_host") {
    html += "<div class='box'><h3>USB host target</h3>";
    html += "<small>This board acts as a real USB host: it automatically enumerates whatever printer is plugged into its native USB port and claims it if it identifies as a USB Printer-class device. Nothing to configure here.</small>";
    html += "</div>";
  }

  html += "<div class='box'><h3>Label</h3>";
  html += "<label>DPI<input name='dpi' value='" + String(cfg.dpi) + "'></label>";
  html += "<label>Label width mm<input name='labelWidth' value='" + String(cfg.labelWidth) + "'></label>";
  html += "<label>Label length mm<input name='labelLength' value='" + String(cfg.labelLength) + "'></label>";
  html += "<small>0 = endless label (continuous roll, no fixed length).</small>";
  if (cfg.printerProtocol != "ql" && cfg.printerProtocol != "seiko") {
    html += "<label><input type='checkbox' name='zplCompressionSupported' ";
    if (cfg.zplCompressionSupported) html += "checked";
    html += "> Printer supports compressed graphics (:Z64:/:B64:)</label>";
    html += "<small>Not every ZPL printer implements this. Only enable if you've confirmed image labels print correctly with it on -- if unsure, leave off.</small>";
  }
  html += "</div>";

  if (cfg.printerProtocol == "ql") {
    html += "<div class='box'><h3>Brother QL raster</h3>";
    html += "<label>Printhead width px (720 = standard family, 1296 = QL-11xx wide)<input name='qlPrintheadPx' value='" + String(cfg.qlPrintheadPx) + "'></label>";
    html += "<label>Invalidate bytes (200 = most models, 400 = QL-800/810W/820NWB)<input name='qlInvalidateBytes' value='" + String(cfg.qlInvalidateBytes) + "'></label>";
    html += "<label><input type='checkbox' name='qlAutoCut' ";
    if (cfg.qlAutoCut) html += "checked";
    html += "> Auto-cut after each label</label>";
    html += "<label>Feed margin (dots)<input name='qlFeedMarginDots' value='" + String(cfg.qlFeedMarginDots) + "'></label>";
    html += "<label>Right margin (dots, 0 = auto-center)<input name='qlRightMarginDots' value='" + String(cfg.qlRightMarginDots) + "'></label>";
    html += "<small>This firmware doesn't implement Brother's exact per-media offset table, dithering, red/black two-color, 600dpi mode, or reading the printer's status response back -- image jobs are resized to Label width/length above at native 300dpi, thresholded to black/white, and centered (or offset per Right margin). See stikka-esp32/README.md.</small>";
    html += "</div>";
  }

  if (cfg.printerProtocol == "seiko") {
    html += "<div class='box'><h3>Seiko SLP raster</h3>";
    html += "<label>Max dots per line (576 = SLP-650)<input name='seikoMaxDots' value='" + String(cfg.seikoMaxDots) + "'></label>";
    html += "<label>Print density (0-255, 0 = 100%)<input name='seikoDensity' value='" + String(cfg.seikoDensity) + "'></label>";
    html += "<label>Print speed (0 = draft, 2 = fine/300dpi)<input name='seikoSpeed' value='" + String(cfg.seikoSpeed) + "'></label>";
    html += "<small>Image jobs are resized to Label width/length above at the DPI set there, thresholded to black/white, and centered on the printhead via the printer's own margin command -- no dithering, no per-model lookup table. See stikka-esp32/README.md.</small>";
    html += "</div>";
  }

  html += "<div class='box'><h3>Debug output</h3>";
  html += "<label><input type='checkbox' name='debugOutput' ";
  if (cfg.debugOutput) html += "checked";
  html += "> Enable serial debug output</label>";
  html += "<small>Always available on this board's programming/USB port (115200 8N1), regardless of printer connection method -- the status LED's mode/pin/color are fixed by the firmware build, not editable here.</small>";
  html += "</div>";
  html += "</div>";

  html += "<button type='submit'>Save and reconnect</button>";
  html += "</form>";

  html += "<form method='POST' action='/test'><button type='submit'>Send test print</button></form>";
  html += wifiScanScriptTag();
  html += "</body></html>";

  return html;
}

static String renderLogsPage() {
  String html;
  html.reserve(4000);
  html += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Stikka ESP32 - Logs</title><style>";
  html += "body{font-family:Arial,sans-serif;max-width:960px;margin:1rem auto;padding:0 1rem;}";
  html += "h1{margin-bottom:.25rem;} nav{margin-bottom:1rem;} nav a{margin-right:1rem;font-weight:600;text-decoration:none;color:#333;}";
  html += "select,button{padding:.4rem .6rem;} .toolbar{display:flex;gap:1rem;align-items:center;flex-wrap:wrap;margin-bottom:1rem;}";
  html += "#log{border:1px solid #ddd;border-radius:8px;background:#111;color:#ddd;font-family:Consolas,Menlo,monospace;";
  html += "font-size:.85rem;padding:.75rem;height:60vh;overflow-y:auto;white-space:pre-wrap;word-break:break-word;}";
  html += ".lvl-ERROR{color:#ff6b6b;} .lvl-WARN{color:#ffd166;} .lvl-INFO{color:#8ecae6;} .lvl-DEBUG{color:#999;}";
  html += "</style></head><body>";
  html += "<nav><a href='/'>Config</a><a href='/logs'>Logs</a></nav>";
  html += "<h1>Device Logs</h1>";

  html += "<div class='toolbar'>";
  html += "<form method='POST' action='/logs/level' style='display:inline;'>";
  html += "<label>Log level ";
  html += "<select name='level' onchange='this.form.submit()'>";
  static const char* kLevels[] = {"ERROR", "WARN", "INFO", "DEBUG"};
  for (const char* lvl : kLevels) {
    html += "<option value='" + String(lvl) + "'";
    if (String(logLevelName(cfg.logLevel)) == lvl) html += " selected";
    html += ">" + String(lvl) + "</option>";
  }
  html += "</select></label></form>";
  html += "<label><input type='checkbox' id='autoscroll' checked> Auto-scroll</label>";
  html += "<label><input type='checkbox' id='autorefresh' checked> Auto-refresh</label>";
  html += "<form method='POST' action='/logs/clear' style='display:inline;'><button type='submit'>Clear</button></form>";
  html += "</div>";

  html += "<div id='log'></div>";

  html += "<script>";
  html += "let sinceSeq=0;const logEl=document.getElementById('log');";
  html += "function fmtMs(ms){const s=Math.floor(ms/1000);return '['+String(Math.floor(s/3600)).padStart(2,'0')+':'+String(Math.floor((s%3600)/60)).padStart(2,'0')+':'+String(s%60).padStart(2,'0')+']';}";
  html += "async function poll(){";
  html += "try{const res=await fetch('/logs.json?since='+sinceSeq);const data=await res.json();";
  html += "for(const e of data.entries){sinceSeq=Math.max(sinceSeq,e.seq);";
  html += "const line=document.createElement('div');line.className='lvl-'+e.level;";
  html += "line.textContent=fmtMs(e.ms)+' '+e.level+' '+e.msg;logEl.appendChild(line);}";
  html += "if(data.entries.length && document.getElementById('autoscroll').checked){logEl.scrollTop=logEl.scrollHeight;}";
  html += "}catch(e){}";
  html += "setTimeout(poll, document.getElementById('autorefresh').checked?1500:4000);";
  html += "}";
  html += "poll();";
  html += "</script>";

  html += "</body></html>";
  return html;
}

static void handleRoot() {
  if (!setupWizardComplete()) {
    web.send(200, "text/html", renderWizardPage());
    return;
  }
  web.send(200, "text/html", renderConfigPage());
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

static void handleSave() {
  cfg.wifiSsid = web.arg("wifiSsid");
  cfg.wifiPassword = web.arg("wifiPassword");
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

static void handleTest() {
  if (cfg.printerProtocol == "ql" || cfg.printerProtocol == "seiko") {
    // No client-supplied image exists for a firmware-triggered test print,
    // so this builds a small synthetic striped raster (see test_print.cpp)
    // instead of the raw ZPL string the zpl branch below sends -- same
    // wire framing a real job would use, just placeholder row content.
    std::unique_ptr<uint8_t[]> bytes;
    size_t len = 0;
    const bool built = (cfg.printerProtocol == "ql")
        ? buildQlTestPrintBytes(bytes, len)
        : buildSeikoTestPrintBytes(bytes, len);
    if (!built) {
      web.send(500, "text/plain", "test failed: could not build test pattern (out of memory)");
      return;
    }
    String err;
    const bool ok = targetSend(bytes.get(), len, err);
    if (!ok) {
      dbgPrintln("[test] test print failed: " + err, LogLevel::LOG_ERROR);
      web.send(500, "text/plain", "test failed: " + err);
      return;
    }
    dbgPrintln("[test] test label sent", LogLevel::LOG_INFO);
    web.send(200, "text/plain", "test label sent");
    return;
  }
  String err;
  const String zpl = "^XA^CF0,30^FO40,40^FDStikka ESP32 test^FS^XZ";
  const bool ok = targetSendString(zpl, err);
  if (!ok) {
    dbgPrintln("[test] test print failed: " + err, LogLevel::LOG_ERROR);
    web.send(500, "text/plain", "test failed: " + err);
    return;
  }
  dbgPrintln("[test] test label sent", LogLevel::LOG_INFO);
  web.send(200, "text/plain", "test label sent");
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
  web.on("/wifi-scan/start", HTTP_POST, handleWifiScanStart);
  web.on("/wifi-scan/result", HTTP_GET, handleWifiScanResult);
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
