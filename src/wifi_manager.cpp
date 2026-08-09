#include "wifi_manager.h"

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include "config.h"
#include "logging.h"
#include "mqtt_bridge.h"

static DNSServer dnsServer;
static bool dnsServerActive = false;
static bool apModeActive = false;
static bool mdnsActive = false;
static String mdnsHostname;
static unsigned long lastWifiAttemptMs = 0;
static unsigned long wifiDisconnectedSinceMs = 0;
static int lastWifiStatus = -1;
static bool lastApModeState = false;

static const unsigned long WIFI_RETRY_EVERY_MS = 5000;
static const unsigned long WIFI_FALLBACK_AP_AFTER_MS = 20000;
static const char* AP_PASSWORD = "stikkaesp32";
static const uint16_t DNS_PORT = 53;
// Overrides the ESP32 Arduino core's default soft-AP IP (192.168.4.1).
static const IPAddress AP_IP(1, 2, 3, 4);
static const IPAddress AP_SUBNET(255, 255, 255, 0);

bool wifiIsConnected() {
  return WiFi.status() == WL_CONNECTED;
}

bool wifiApModeActive() {
  return apModeActive;
}

const char* wifiApPassword() {
  return AP_PASSWORD;
}

String wifiFallbackApSsid() {
  uint32_t mac = (uint32_t)ESP.getEfuseMac();
  String suffix = String(mac, HEX);
  suffix.toUpperCase();
  return String("Stikka-") + suffix;
}

// mDNS hostnames are DNS labels: letters/digits/hyphens only, no leading or
// trailing hyphen. cfg.printerName is freeform user text (default
// "stikka-esp32", but web-UI-editable to anything), so it needs sanitizing
// before use here rather than assuming it's already a valid label.
static String sanitizeMdnsHostname(const String& raw) {
  String out;
  out.reserve(raw.length());
  for (size_t i = 0; i < raw.length(); i++) {
    char c = raw[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
      out += c;
    } else if (out.length() > 0 && out[out.length() - 1] != '-') {
      out += '-';
    }
  }
  while (out.length() > 0 && out[0] == '-') out.remove(0, 1);
  while (out.length() > 0 && out[out.length() - 1] == '-') out.remove(out.length() - 1, 1);
  if (out.isEmpty()) out = "stikka-esp32";
  return out;
}

static void stopMdns() {
  if (!mdnsActive) return;
  MDNS.end();
  mdnsActive = false;
  mdnsHostname = "";
}

String wifiMdnsHostname() {
  return mdnsActive ? mdnsHostname : String();
}

// Restarted (not just started once) on every fresh station connection --
// covers both a plain reconnect and cfg.printerName having changed via the
// web UI (handleSave() forces a Wi-Fi disconnect/reconnect cycle after
// saving, so the new name takes effect here without a separate hook).
static void startMdns() {
  stopMdns();
  const String hostname = sanitizeMdnsHostname(cfg.printerName);
  if (!MDNS.begin(hostname.c_str())) {
    dbgPrintln("[mdns] responder failed to start", LogLevel::LOG_WARN);
    return;
  }
  MDNS.addService("http", "tcp", 80);
  mdnsActive = true;
  mdnsHostname = hostname;
  // LOG_ERROR here for the same reason as the "[wifi] connected, ip=" line
  // above: guaranteed to survive even the quietest configured log level,
  // since this -- like the IP -- is how you find the device.
  dbgPrint("[mdns] responder started, browse to http://");
  dbgPrint(hostname);
  dbgPrintln(".local/", LogLevel::LOG_ERROR);
}

static void ensureFallbackAp() {
  if (apModeActive) return;
  // Explicit, not assumed left over from wifiManagerSetup(): dropToStaOnly()
  // below can turn this back down to WIFI_STA whenever a connected device
  // never needed the AP, so it has to be re-armed here every time.
  WiFi.mode(WIFI_AP_STA);
  const String ssid = wifiFallbackApSsid();
  if (WiFi.softAP(ssid.c_str(), AP_PASSWORD)) {
    WiFi.softAPConfig(AP_IP, AP_IP, AP_SUBNET);
    apModeActive = true;
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    dnsServerActive = true;
    dbgPrint("[wifi] fallback AP started: ");
    dbgPrint(ssid);
    dbgPrint(" (ip=");
    dbgPrint(WiFi.softAPIP());
    dbgPrintln(")", LogLevel::LOG_WARN);
  }
}

// Drops the radio to WIFI_STA (no AP interface at all) once we know the
// fallback AP isn't needed. Distinct from wifiDisableFallbackAp() below:
// that one only runs its teardown when apModeActive is true, i.e. only after
// *this* code explicitly started an AP via ensureFallbackAp() -- a device
// that connects to station Wi-Fi on its very first attempt never sets
// apModeActive, but wifiManagerSetup() still leaves it in WIFI_AP_STA from
// boot. Left alone, the SoftAP interface stays enabled with the SDK's
// default/unconfigured AP config, which broadcasts a stray "ESP-<hex>"
// network for as long as the device is up -- observed in the field, one per
// already-connected device. Safe to call unconditionally on every confirmed
// station connection regardless of AP history.
static void dropToStaOnly() {
  if (WiFi.getMode() != WIFI_STA) {
    WiFi.mode(WIFI_STA);
  }
}

void wifiDisableFallbackAp() {
  if (!apModeActive) return;
  if (dnsServerActive) {
    dnsServer.stop();
    dnsServerActive = false;
  }
  WiFi.softAPdisconnect(true);
  apModeActive = false;
  dbgPrintln("[wifi] fallback AP disabled", LogLevel::LOG_INFO);
}

static void connectWifi() {
  const unsigned long now = millis();

  if (cfg.wifiSsid.isEmpty()) {
    ensureFallbackAp();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiDisconnectedSinceMs = 0;
    wifiDisableFallbackAp();
    dropToStaOnly();
    return;
  }

  if (wifiDisconnectedSinceMs == 0) {
    wifiDisconnectedSinceMs = now;
  }

  if (now - wifiDisconnectedSinceMs >= WIFI_FALLBACK_AP_AFTER_MS) {
    ensureFallbackAp();
  }

  if (now - lastWifiAttemptMs < WIFI_RETRY_EVERY_MS) return;
  lastWifiAttemptMs = now;

  WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPassword.c_str());
}

void wifiForceDisconnect() {
  WiFi.disconnect(true);
  wifiDisconnectedSinceMs = millis();
}

void printNetworkState(const char* reason) {
  dbgPrintln();
  dbgPrintln("---------------- Network State ----------------");
  dbgPrint("Reason: ");
  dbgPrintln(reason);
  const int wifiState = WiFi.status();
  if (wifiState == WL_CONNECTED) {
    dbgPrint("Wi-Fi: connected to '");
    dbgPrint(WiFi.SSID());
    dbgPrintln("'");
    dbgPrint("IP: ");
    dbgPrintln(WiFi.localIP());
    dbgPrint("Gateway: ");
    dbgPrintln(WiFi.gatewayIP());
    dbgPrint("RSSI: ");
    dbgPrint(WiFi.RSSI());
    dbgPrintln(" dBm");
    if (mdnsActive) {
      dbgPrint("mDNS: http://");
      dbgPrint(mdnsHostname);
      dbgPrintln(".local/");
    }
  } else {
    dbgPrint("Wi-Fi: disconnected (status=");
    dbgPrint(wifiState);
    dbgPrintln(")");
  }

  if (apModeActive) {
    dbgPrint("Fallback AP: active, SSID='");
    dbgPrint(wifiFallbackApSsid());
    dbgPrintln("'");
    dbgPrint("AP IP: ");
    dbgPrintln(WiFi.softAPIP());
  } else {
    dbgPrintln("Fallback AP: inactive");
  }

  dbgPrint("MQTT: ");
  dbgPrintln(mqttIsConnected() ? "connected" : "disconnected");
  dbgPrintln("-----------------------------------------------");
  dbgPrintln();
}

void wifiStartScan() {
  if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) return;
  WiFi.scanDelete();
  WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);
}

String wifiScanResultJson() {
  const int16_t n = WiFi.scanComplete();
  JsonDocument doc;

  if (n == WIFI_SCAN_RUNNING) {
    doc["status"] = "scanning";
  } else if (n == WIFI_SCAN_FAILED) {
    doc["status"] = "failed";
  } else if (n < 0) {
    doc["status"] = "idle";
  } else {
    doc["status"] = "done";
    JsonArray arr = doc["networks"].to<JsonArray>();
    // n is small (real-world scans rarely exceed a few dozen APs), so this
    // repeated linear scan to pick strongest-first while skipping
    // already-listed SSIDs (mesh routers commonly broadcast the same name
    // from multiple APs) is cheap -- not worth a sort/dedup data structure
    // for a one-off web request.
    for (int rank = 0; rank < n; rank++) {
      int best = -1;
      int32_t bestRssi = INT32_MIN;
      for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i).isEmpty()) continue;
        bool alreadyListed = false;
        for (JsonObject existing : arr) {
          if (existing["ssid"] == WiFi.SSID(i)) {
            alreadyListed = true;
            break;
          }
        }
        if (alreadyListed) continue;
        if (WiFi.RSSI(i) > bestRssi) {
          bestRssi = WiFi.RSSI(i);
          best = i;
        }
      }
      if (best < 0) break;
      JsonObject net = arr.add<JsonObject>();
      net["ssid"] = WiFi.SSID(best);
      net["rssi"] = WiFi.RSSI(best);
      net["secure"] = WiFi.encryptionType(best) != WIFI_AUTH_OPEN;
    }
    WiFi.scanDelete();
  }

  String out;
  serializeJson(doc, out);
  return out;
}

void wifiManagerSetup() {
  WiFi.mode(WIFI_AP_STA);
  connectWifi();
}

void wifiManagerLoop() {
  if (dnsServerActive) {
    dnsServer.processNextRequest();
  }

  connectWifi();

  const int wifiNow = WiFi.status();
  if (wifiNow != lastWifiStatus) {
    lastWifiStatus = wifiNow;
    if (wifiNow == WL_CONNECTED) {
      // LOG_ERROR here isn't about severity -- it's the one level guaranteed
      // to show regardless of the configured log level (see flushPendingLogLine:
      // a line is dropped only if it's MORE verbose than cfg.logLevel, and
      // nothing is more severe than ERROR). The IP is how you find the device
      // to reconfigure it, so it needs to survive even the quietest setting.
      dbgPrint("[wifi] connected, ip=");
      dbgPrintln(WiFi.localIP(), LogLevel::LOG_ERROR);
      startMdns();
      printNetworkState("wifi connected");
    } else {
      dbgPrintln("[wifi] disconnected", LogLevel::LOG_ERROR);
      stopMdns();
      printNetworkState("wifi disconnected");
    }
  }

  if (apModeActive != lastApModeState) {
    lastApModeState = apModeActive;
    printNetworkState(apModeActive ? "fallback AP enabled" : "fallback AP disabled");
  }
}
