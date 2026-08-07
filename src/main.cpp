#include <Arduino.h>

#include "config.h"
#include "logging.h"
#include "mqtt_bridge.h"
#include "status_led.h"
#include "targets/target.h"
#include "web_ui.h"
#include "wifi_manager.h"

void setup() {
  loadConfig();
  applyDebugOutputSetting(cfg.debugOutput);
  setupStatusLed();
  targetSetup();
  mqttBridgeSetup();
  printRuntimeSettings("boot/reset");
  dbgPrintln("[boot] Stikka ESP32 bridge starting", LogLevel::LOG_INFO);
  // Confirms PSRAM actually initialized (board_build.arduino.memory_type in
  // platformio.ini) -- worth seeing on every boot of a board that's expected
  // to have it, not just when debugging a specific issue.
  if (psramFound()) {
    dbgPrint("[boot] psram found, size=");
    dbgPrintln((unsigned long)ESP.getPsramSize(), LogLevel::LOG_INFO);
  } else {
    dbgPrintln("[boot] no psram found", LogLevel::LOG_INFO);
  }

  wifiManagerSetup();
  webUiSetup();
  dbgPrintln("[boot] web UI ready", LogLevel::LOG_INFO);
  printNetworkState("startup");
}

void loop() {
  wifiManagerLoop();
  webUiLoop();
  mqttBridgeLoop();
  targetLoop();
  updateStatusLed(wifiIsConnected(), mqttIsConnected());
}
