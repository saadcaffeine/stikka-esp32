#include "web_ui.h"

#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "logging.h"
#include "mqtt_bridge.h"
#include "status_led.h"
#include "targets/target.h"
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

  html += "<form method='POST' action='/save'>";
  html += "<div class='grid'>";
  html += "<div class='box'><h3>Wi-Fi &amp; Printer Name</h3>";
  html += "<label>SSID<input name='wifiSsid' value='" + htmlEscape(cfg.wifiSsid) + "'></label>";
  html += "<label>Password<input type='password' name='wifiPassword' value='" + htmlEscape(cfg.wifiPassword) + "'></label>";
  html += "<label>Printer name<input name='printerName' value='" + htmlEscape(cfg.printerName) + "'></label>";
  html += "<small>Type: <code>" PRINTER_TYPE "</code> · transport: <code>";
  html += targetMethodName();
  html += "</code> (both fixed by this firmware build) · status topic: /&lt;printername&gt;/status/ · command topic: /&lt;printername&gt;/command/</small>";
  html += "<label>Location (optional)<input name='location' value='" + htmlEscape(cfg.location) + "'></label>";
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

#if defined(TARGET_NETWORK)
  html += "<div class='box'><h3>Network target</h3>";
  html += "<label>Target host<input name='zplTargetHost' value='" + htmlEscape(cfg.zplTargetHost) + "'></label>";
  html += "<label>Target port<input name='zplTargetPort' value='" + String(cfg.zplTargetPort) + "'></label>";
  html += "</div>";
#elif defined(TARGET_SERIAL)
  html += "<div class='box'><h3>Serial target (UART)</h3>";
  html += "<label>TX pin<input name='printerUartTxPin' value='" + String(cfg.printerUartTxPin) + "'></label>";
  html += "<label>RX pin<input name='printerUartRxPin' value='" + String(cfg.printerUartRxPin) + "'></label>";
  html += "<label>Baud rate<input name='printerUartBaud' value='" + String(cfg.printerUartBaud) + "'></label>";
  html += "<small>A dedicated hardware UART (not the USB/programming port) wired directly to the printer's serial input, 8N1.</small>";
  html += "</div>";
#elif defined(TARGET_USB)
  html += "<div class='box'><h3>USB target</h3>";
  html += "<label>Baud rate<input name='printerUsbBaud' value='" + String(cfg.printerUsbBaud) + "'></label>";
  html += "<small>Reuses this board's USB/programming port to send print data over the same cable. Serial debug output can't also use usb mode on this build (it would corrupt the print stream on the shared port) -- use uart mode below if you need serial logs.</small>";
  html += "</div>";
#elif defined(TARGET_USB_HOST)
  html += "<div class='box'><h3>USB host target</h3>";
  html += "<small>This board acts as a real USB host: it automatically enumerates whatever printer is plugged into its native USB port and claims it if it identifies as a USB Printer-class device. Nothing to configure here. Serial debug output can't use usb mode on this build either (the native USB peripheral is claimed for host mode) -- use uart mode below if you need serial logs.</small>";
  html += "</div>";
#endif

  html += "<div class='box'><h3>Label</h3>";
  html += "<label>DPI<input name='dpi' value='" + String(cfg.dpi) + "'></label>";
  html += "<label>Label width mm<input name='labelWidth' value='" + String(cfg.labelWidth) + "'></label>";
  html += "<label>Label length mm<input name='labelLength' value='" + String(cfg.labelLength) + "'></label>";
  html += "<small>0 = endless label (continuous roll, no fixed length).</small>";
#if !defined(PROTOCOL_QL)
  html += "<label><input type='checkbox' name='zplCompressionSupported' ";
  if (cfg.zplCompressionSupported) html += "checked";
  html += "> Printer supports compressed graphics (:Z64:/:B64:)</label>";
  html += "<small>Not every ZPL printer implements this. Only enable if you've confirmed image labels print correctly with it on -- if unsure, leave off.</small>";
#endif
  html += "</div>";

#ifdef PROTOCOL_QL
  html += "<div class='box'><h3>Brother QL raster</h3>";
  html += "<label>Printhead width px (720 = standard family, 1296 = QL-11xx wide)<input name='qlPrintheadPx' value='" + String(cfg.qlPrintheadPx) + "'></label>";
  html += "<label>Invalidate bytes (200 = most models, 400 = QL-800/810W/820NWB)<input name='qlInvalidateBytes' value='" + String(cfg.qlInvalidateBytes) + "'></label>";
  html += "<label><input type='checkbox' name='qlAutoCut' ";
  if (cfg.qlAutoCut) html += "checked";
  html += "> Auto-cut after each label</label>";
  html += "<label>Feed margin (dots)<input name='qlFeedMarginDots' value='" + String(cfg.qlFeedMarginDots) + "'></label>";
  html += "<label>Right margin (dots, 0 = auto-center)<input name='qlRightMarginDots' value='" + String(cfg.qlRightMarginDots) + "'></label>";
  html += "<small>This firmware doesn't implement Brother's exact per-media offset table, dithering, red/black two-color, 600dpi mode, or reading the printer's status response back -- image jobs are resized to Label width/length above at native 300dpi, thresholded to black/white, and centered (or offset per Right margin). See esp32/README.md.</small>";
  html += "</div>";
#endif

  html += "<div class='box'><h3>Debug + Status LED</h3>";
  html += "<label><input type='checkbox' name='debugOutput' ";
  if (cfg.debugOutput) html += "checked";
  html += "> Enable serial debug output</label>";
#if defined(TARGET_USB) || defined(USB_HOST_SHARES_DEBUG_PORT)
  html += "<label>Debug output mode<input name='debugOutputMode' value='uart' readonly></label>";
  html += "<small>Fixed to uart on this build -- usb mode's port is reserved for the printer connection (see USB target above), or (usb_host builds) shares this board's native USB connector with the printer connection.</small>";
#else
  html += "<label>Debug output mode (usb / uart)<input name='debugOutputMode' value='" + htmlEscape(cfg.debugOutputMode) + "'></label>";
  html += "<small>Use usb for logs over USB serial, or uart for logs on custom TX/RX pins (115200 8N1).</small>";
#endif
  html += "<label>Debug UART TX pin<input name='debugUartTxPin' value='" + String(cfg.debugUartTxPin) + "'></label>";
  html += "<label>Debug UART RX pin<input name='debugUartRxPin' value='" + String(cfg.debugUartRxPin) + "'></label>";
  html += "<label>LED mode (none / neopixel / rgb)<input name='ledMode' value='" + htmlEscape(cfg.ledMode) + "'></label>";
  html += "<label>NeoPixel data pin<input name='ledPin' value='" + String(cfg.ledPin) + "'></label>";
  html += "<label>RGB pin R<input name='ledPinR' value='" + String(cfg.ledPinR) + "'></label>";
  html += "<label>RGB pin G<input name='ledPinG' value='" + String(cfg.ledPinG) + "'></label>";
  html += "<label>RGB pin B<input name='ledPinB' value='" + String(cfg.ledPinB) + "'></label>";
  html += "<label>LED color order (RGB/GRB/... )<input name='ledOrder' value='" + htmlEscape(cfg.ledOrder) + "'></label>";
  html += "<label>LED blink speed (ms)<input name='ledBlinkMs' value='" + String(cfg.ledBlinkMs) + "'></label>";
  html += "<small>States: green=WiFi+MQTT, yellow=WiFi only, red=no WiFi/AP, purple=MQTT RX, cyan=MQTT TX.</small>";
  html += "</div>";
  html += "</div>";

  html += "<button type='submit'>Save and reconnect</button>";
  html += "</form>";

#ifdef PROTOCOL_QL
  html += "<form method='POST' action='/test'><button type='submit' disabled title='not implemented for the ql protocol yet'>Send test ZPL (unavailable)</button></form>";
#else
  html += "<form method='POST' action='/test'><button type='submit'>Send test ZPL</button></form>";
#endif
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
#if defined(TARGET_NETWORK)
  cfg.zplTargetHost = web.arg("zplTargetHost");
  cfg.zplTargetPort = parsePort(web.arg("zplTargetPort"), 9100);
#elif defined(TARGET_SERIAL)
  cfg.printerUartTxPin = web.arg("printerUartTxPin").toInt();
  cfg.printerUartRxPin = web.arg("printerUartRxPin").toInt();
  cfg.printerUartBaud = (uint32_t)web.arg("printerUartBaud").toInt();
  if (cfg.printerUartBaud == 0) cfg.printerUartBaud = 9600;
#elif defined(TARGET_USB)
  cfg.printerUsbBaud = (uint32_t)web.arg("printerUsbBaud").toInt();
  if (cfg.printerUsbBaud == 0) cfg.printerUsbBaud = 115200;
#elif defined(TARGET_USB_HOST)
  // Nothing to save -- the printer is auto-enumerated, no fields posted.
#endif
  cfg.dpi = web.arg("dpi").toInt();
  if (cfg.dpi <= 0) cfg.dpi = DEFAULT_DPI;
  cfg.labelWidth = web.arg("labelWidth").toInt();
  if (cfg.labelWidth <= 0) cfg.labelWidth = 55;
  cfg.labelLength = web.arg("labelLength").toInt();
  // 0 is a valid, deliberate value here: it means "endless label" (continuous
  // roll, no fixed length) throughout the frontend (zpl-image.ts, mqtt-api.ts,
  // editor.ts, ui.ts). Only negative input is nonsense and falls back to the default.
  if (cfg.labelLength < 0) cfg.labelLength = 55;
#if !defined(PROTOCOL_QL)
  cfg.zplCompressionSupported = web.hasArg("zplCompressionSupported");
#endif

#ifdef PROTOCOL_QL
  cfg.qlPrintheadPx = web.arg("qlPrintheadPx").toInt() == 1296 ? 1296 : 720;
  cfg.qlInvalidateBytes = web.arg("qlInvalidateBytes").toInt();
  if (cfg.qlInvalidateBytes <= 0) cfg.qlInvalidateBytes = 200;
  cfg.qlAutoCut = web.hasArg("qlAutoCut");
  cfg.qlFeedMarginDots = web.arg("qlFeedMarginDots").toInt();
  if (cfg.qlFeedMarginDots < 0) cfg.qlFeedMarginDots = 35;
  cfg.qlRightMarginDots = web.arg("qlRightMarginDots").toInt();
  if (cfg.qlRightMarginDots < 0) cfg.qlRightMarginDots = 0;
#endif

  cfg.debugOutput = web.hasArg("debugOutput");
#if defined(TARGET_USB) || defined(USB_HOST_SHARES_DEBUG_PORT)
  // TARGET_USB reserves Serial/UART0 for the printer connection itself
  // (targets/usb_target.cpp); USB_HOST_SHARES_DEBUG_PORT is set only for
  // boards where usb_host's native USB connector is electrically the same
  // one the debug-UART bridge chip uses (see platformio.ini) -- either way,
  // debug output can't share it, regardless of what the (readonly, on these
  // builds) form field posted.
  cfg.debugOutputMode = "uart";
#else
  cfg.debugOutputMode = web.arg("debugOutputMode");
  cfg.debugOutputMode.toLowerCase();
  if (cfg.debugOutputMode != "usb" && cfg.debugOutputMode != "uart") {
    cfg.debugOutputMode = DEFAULT_DEBUG_OUTPUT_MODE;
  }
#endif
  cfg.debugUartTxPin = web.arg("debugUartTxPin").toInt();
  cfg.debugUartRxPin = web.arg("debugUartRxPin").toInt();
  cfg.ledMode = web.arg("ledMode");
  cfg.ledMode.toLowerCase();
  if (cfg.ledMode != "none" && cfg.ledMode != "neopixel" && cfg.ledMode != "rgb") {
    cfg.ledMode = "none";
  }
  cfg.ledPin = web.arg("ledPin").toInt();
  cfg.ledPinR = web.arg("ledPinR").toInt();
  cfg.ledPinG = web.arg("ledPinG").toInt();
  cfg.ledPinB = web.arg("ledPinB").toInt();
  cfg.ledOrder = web.arg("ledOrder");
  cfg.ledOrder.toUpperCase();
  cfg.ledBlinkMs = (uint16_t)parsePort(web.arg("ledBlinkMs"), 700);
  if (cfg.ledBlinkMs < 100) cfg.ledBlinkMs = 100;
  if (cfg.ledBlinkMs > 5000) cfg.ledBlinkMs = 5000;

  if (cfg.printerName.isEmpty()) cfg.printerName = "stikka-esp32";

  saveConfig();
  applyDebugOutputSetting(cfg.debugOutput);
  setupStatusLed();
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
#ifdef PROTOCOL_QL
  // A raw ZPL string is meaningless to a QL raster printer -- there's no
  // synthetic-image test pattern wired up yet, so this button is a no-op on
  // this build rather than sending bytes the printer can't interpret.
  web.send(501, "text/plain", "test print isn't implemented for the ql protocol yet -- send a real print job from the app");
  return;
#else
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
#endif
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

void webUiSetup() {
  web.on("/", HTTP_GET, handleRoot);
  web.on("/generate_204", HTTP_GET, handleCaptivePortalRedirect);
  web.on("/gen_204", HTTP_GET, handleCaptivePortalRedirect);
  web.on("/hotspot-detect.html", HTTP_GET, handleCaptivePortal);
  web.on("/connecttest.txt", HTTP_GET, handleCaptivePortal);
  web.on("/ncsi.txt", HTTP_GET, handleCaptivePortal);
  web.on("/fwlink", HTTP_GET, handleCaptivePortalRedirect);
  web.on("/save", HTTP_POST, handleSave);
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
