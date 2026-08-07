#pragma once

#include <Arduino.h>

// Compile-time defaults, overridable per platformio.ini env via build_flags
// (e.g. -D DEFAULT_MQTT_HOST='"host"'). These only take effect on first boot
// / after an NVS erase -- loadConfig() prefers whatever's already saved in
// Preferences, since these are meant to seed a fresh device, not override one
// a user already configured through the web UI.
#ifndef DEFAULT_MQTT_HOST
#define DEFAULT_MQTT_HOST ""
#endif
#ifndef DEFAULT_MQTT_PORT
#define DEFAULT_MQTT_PORT 1883
#endif
#ifndef DEFAULT_MQTT_USE_TLS
#define DEFAULT_MQTT_USE_TLS true
#endif
#ifndef DEFAULT_MQTT_TLS_INSECURE
#define DEFAULT_MQTT_TLS_INSECURE true
#endif
#ifndef DEFAULT_MQTT_USER
#define DEFAULT_MQTT_USER ""
#endif
#ifndef DEFAULT_MQTT_PASSWORD
#define DEFAULT_MQTT_PASSWORD ""
#endif
#ifndef DEFAULT_LED_MODE
#define DEFAULT_LED_MODE "none"
#endif
#ifndef DEFAULT_LED_PIN
#define DEFAULT_LED_PIN -1
#endif
#ifndef DEFAULT_LED_ORDER
#define DEFAULT_LED_ORDER "GRB"
#endif
#ifndef DEFAULT_DPI
#define DEFAULT_DPI 203
#endif
#ifndef DEFAULT_DEBUG_OUTPUT_MODE
#define DEFAULT_DEBUG_OUTPUT_MODE "usb"
#endif
// GPIO16/17 are the generic ESP32 default for the debug UART, but they're
// unusable on ESP32-PICO-D4-based boards (M5Stack Atom among them) -- that
// package wires those two pins internally to its embedded flash, so
// HardwareSerial.begin() on them corrupts flash access and bootloops the
// board. Boards built on PICO-D4 must override both via build_flags.
#ifndef DEFAULT_DEBUG_UART_TX_PIN
#define DEFAULT_DEBUG_UART_TX_PIN 17
#endif
#ifndef DEFAULT_DEBUG_UART_RX_PIN
#define DEFAULT_DEBUG_UART_RX_PIN 16
#endif

// Printer protocol type is fixed by the firmware build (env naming already
// encodes it, e.g. esp32-s3-devkitc-1_zpl_network), so unlike the DEFAULT_* knobs
// above it isn't NVS-backed or web-UI-editable -- a future non-ZPL protocol
// gets its own value via build_flags on its own env(s), not a runtime toggle.
#ifndef PRINTER_TYPE
#define PRINTER_TYPE "zpl"
#endif

enum class LogLevel : uint8_t {
  LOG_ERROR = 0,
  LOG_WARN = 1,
  LOG_INFO = 2,
  LOG_DEBUG = 3,
};

const char* logLevelName(LogLevel level);
LogLevel logLevelFromString(const String& raw, LogLevel fallback);

struct AppConfig {
  String wifiSsid;
  String wifiPassword;
  String mqttHost = DEFAULT_MQTT_HOST;
  uint16_t mqttPort = DEFAULT_MQTT_PORT;
  bool mqttUseTls = DEFAULT_MQTT_USE_TLS;
  bool mqttTlsInsecure = DEFAULT_MQTT_TLS_INSECURE;
  String mqttCaCert;
  String mqttUser = DEFAULT_MQTT_USER;
  String mqttPassword = DEFAULT_MQTT_PASSWORD;
  uint16_t statusIntervalSec = 30;
  String printerName = "stikka-esp32";
  String location; // optional, freeform (e.g. "Front desk") -- shown in the frontend next to the serial
  String zplTargetHost; // METHOD="network" only
  uint16_t zplTargetPort = 9100; // METHOD="network" only
  int dpi = DEFAULT_DPI;
  int labelWidth = 55;
  int labelLength = 55;
  bool zplCompressionSupported = false; // printer accepts ^GF :Z64:/:B64: data (PROTOCOL="zpl" only)
  bool debugOutput = true;
  String debugOutputMode = DEFAULT_DEBUG_OUTPUT_MODE; // usb | uart
  int debugUartTxPin = DEFAULT_DEBUG_UART_TX_PIN;
  int debugUartRxPin = DEFAULT_DEBUG_UART_RX_PIN;
  // METHOD="serial": a dedicated hardware UART (not the USB/programming
  // port) wired straight to the printer -- distinct from the debug UART
  // above so print traffic and log traffic never share a wire.
  int printerUartTxPin = 32;
  int printerUartRxPin = 33;
  uint32_t printerUartBaud = 9600;
  // METHOD="usb": reuses the board's USB/programming port (Serial/UART0)
  // to send print data. Mutually exclusive with debugOutputMode=="usb" on
  // this same build -- see logging.cpp's applyDebugOutputSetting().
  uint32_t printerUsbBaud = 115200;
  // PROTOCOL="ql": Brother QL raster protocol knobs. The frontend now builds
  // the whole raster byte stream client-side (frontend/src/zpl-image.ts),
  // reading these via the status/capabilities JSON (mqtt_bridge.cpp) instead
  // of a firmware-side rasterizer using them directly. Deliberately generic
  // rather than a specific-model lookup table.
  int qlPrintheadPx = 720;      // 720 = standard family, 1296 = QL-11xx wide family
  int qlInvalidateBytes = 200;  // 200 = most models, 400 = QL-800/810W/820NWB
  bool qlAutoCut = true;
  int qlFeedMarginDots = 35;
  int qlRightMarginDots = 0;    // 0 = auto-center; >0 = distance from the head's right edge
  String ledMode = DEFAULT_LED_MODE;   // none | neopixel | rgb
  int ledPin = DEFAULT_LED_PIN;         // neopixel data pin
  int ledPinR = -1;              // discrete RGB R pin
  int ledPinG = -1;              // discrete RGB G pin
  int ledPinB = -1;              // discrete RGB B pin
  String ledOrder = DEFAULT_LED_ORDER; // NeoPixel byte order (RGB, GRB, ...)
  uint16_t ledBlinkMs = 700;     // full blink cycle in ms
  LogLevel logLevel = LogLevel::LOG_INFO; // verbosity for serial output + web Logs tab
};

extern AppConfig cfg;

void loadConfig();
void saveConfig();

// Dumps the full config + derived MQTT topics to the log at LOG_INFO,
// bracketed for readability -- called on boot and after a config save so the
// active settings are always visible in the Logs tab / serial output.
void printRuntimeSettings(const char* reason);
