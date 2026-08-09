#pragma once

#include <Arduino.h>

// WiFi.mode(WIFI_AP_STA) + first connect attempt. Call once from setup().
void wifiManagerSetup();

// DNS captive-portal servicing, station reconnect/retry, fallback-AP
// enter/exit, and state-change diagnostics. Call once per loop() iteration.
void wifiManagerLoop();

bool wifiIsConnected();
bool wifiApModeActive();
String wifiFallbackApSsid();
const char* wifiApPassword();

// The mDNS hostname (without ".local") currently registered, derived from
// cfg.printerName -- empty if the responder isn't running (not yet
// connected to a station network, or it failed to start). See
// startMdns()/wifiManagerLoop() in wifi_manager.cpp.
String wifiMdnsHostname();

// Torn down explicitly (rather than left to the next wifiManagerLoop()
// retry) when the web UI saves new Wi-Fi/MQTT settings, so the old
// connection doesn't linger with stale credentials.
void wifiDisableFallbackAp();
void wifiForceDisconnect();

void printNetworkState(const char* reason);

// Kicks off an async WiFi.scanNetworks() -- no-op if a scan is already in
// progress. Non-blocking: results are collected on subsequent
// wifiScanResultJson() calls (the web UI's /wifi-scan/start + poll
// /wifi-scan/result endpoints, see web_ui.cpp), not returned here.
void wifiStartScan();

// {"status":"idle|scanning|failed|done","networks":[{"ssid","rssi","secure"},...]}
// -- networks are de-duplicated by SSID (keeping the strongest RSSI) and
// sorted strongest-first, skipping hidden (empty-SSID) networks. Only
// present when status=="done".
String wifiScanResultJson();
