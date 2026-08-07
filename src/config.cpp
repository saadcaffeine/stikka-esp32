#include "config.h"

#include <Preferences.h>

#include "logging.h"
#include "mqtt_bridge.h"

AppConfig cfg;

static Preferences prefs;

const char* logLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::LOG_ERROR: return "ERROR";
    case LogLevel::LOG_WARN: return "WARN";
    case LogLevel::LOG_INFO: return "INFO";
    default: return "DEBUG";
  }
}

LogLevel logLevelFromString(const String& raw, LogLevel fallback) {
  String v = raw;
  v.toUpperCase();
  if (v == "ERROR") return LogLevel::LOG_ERROR;
  if (v == "WARN") return LogLevel::LOG_WARN;
  if (v == "INFO") return LogLevel::LOG_INFO;
  if (v == "DEBUG") return LogLevel::LOG_DEBUG;
  return fallback;
}

void loadConfig() {
  if (!prefs.begin("stikka", true)) {
    if (prefs.begin("stikka", false)) {
      prefs.end();
    }
    return;
  }
  cfg.wifiSsid = prefs.getString("wifiSsid", "");
  cfg.wifiPassword = prefs.getString("wifiPwd", "");
  cfg.mqttHost = prefs.getString("mqttHost", DEFAULT_MQTT_HOST);
  cfg.mqttPort = prefs.getUShort("mqttPort", DEFAULT_MQTT_PORT);
  cfg.mqttUseTls = prefs.getBool("mqttTls", DEFAULT_MQTT_USE_TLS);
  cfg.mqttTlsInsecure = prefs.getBool("mqttInsec", DEFAULT_MQTT_TLS_INSECURE);
  cfg.mqttCaCert = prefs.getString("mqttCa", "");
  cfg.mqttUser = prefs.getString("mqttUser", DEFAULT_MQTT_USER);
  cfg.mqttPassword = prefs.getString("mqttPwd", DEFAULT_MQTT_PASSWORD);
  cfg.statusIntervalSec = prefs.getUShort("statInt", 30);
  cfg.printerName = prefs.getString("printer", "stikka-esp32");
  cfg.location = prefs.getString("location", "");
  cfg.zplTargetHost = prefs.getString("zplHost", "");
  cfg.zplTargetPort = prefs.getUShort("zplPort", 9100);
  cfg.dpi = prefs.getInt("dpi", DEFAULT_DPI);
  cfg.labelWidth = prefs.getInt("labelW", 55);
  cfg.labelLength = prefs.getInt("labelL", 55);
  cfg.zplCompressionSupported = prefs.getBool("zplCompr", false);
  cfg.debugOutput = prefs.getBool("dbgOut", true);
  cfg.debugOutputMode = prefs.getString("dbgMode", DEFAULT_DEBUG_OUTPUT_MODE);
  cfg.debugUartTxPin = prefs.getInt("dbgTx", DEFAULT_DEBUG_UART_TX_PIN);
  cfg.debugUartRxPin = prefs.getInt("dbgRx", DEFAULT_DEBUG_UART_RX_PIN);
  cfg.printerUartTxPin = prefs.getInt("puTx", 32);
  cfg.printerUartRxPin = prefs.getInt("puRx", 33);
  cfg.printerUartBaud = prefs.getUInt("puBaud", 9600);
  cfg.printerUsbBaud = prefs.getUInt("usbBaud", 115200);
  cfg.qlPrintheadPx = prefs.getInt("qlHeadPx", 720);
  cfg.qlInvalidateBytes = prefs.getInt("qlInval", 200);
  cfg.qlAutoCut = prefs.getBool("qlCut", true);
  cfg.qlFeedMarginDots = prefs.getInt("qlFeed", 35);
  cfg.qlRightMarginDots = prefs.getInt("qlRMarg", 0);
  cfg.ledMode = prefs.getString("ledMode", DEFAULT_LED_MODE);
  cfg.ledPin = prefs.getInt("ledPin", DEFAULT_LED_PIN);
  cfg.ledPinR = prefs.getInt("ledPinR", -1);
  cfg.ledPinG = prefs.getInt("ledPinG", -1);
  cfg.ledPinB = prefs.getInt("ledPinB", -1);
  cfg.ledOrder = prefs.getString("ledOrder", DEFAULT_LED_ORDER);
  cfg.ledBlinkMs = prefs.getUShort("ledBlink", 700);
  cfg.logLevel = (LogLevel)prefs.getUChar("logLvl", (uint8_t)LogLevel::LOG_INFO);
  prefs.end();
}

void saveConfig() {
  prefs.begin("stikka", false);
  prefs.putString("wifiSsid", cfg.wifiSsid);
  prefs.putString("wifiPwd", cfg.wifiPassword);
  prefs.putString("mqttHost", cfg.mqttHost);
  prefs.putUShort("mqttPort", cfg.mqttPort);
  prefs.putBool("mqttTls", cfg.mqttUseTls);
  prefs.putBool("mqttInsec", cfg.mqttTlsInsecure);
  prefs.putString("mqttCa", cfg.mqttCaCert);
  prefs.putString("mqttUser", cfg.mqttUser);
  prefs.putString("mqttPwd", cfg.mqttPassword);
  prefs.putUShort("statInt", cfg.statusIntervalSec);
  prefs.putString("printer", cfg.printerName);
  prefs.putString("location", cfg.location);
  prefs.putString("zplHost", cfg.zplTargetHost);
  prefs.putUShort("zplPort", cfg.zplTargetPort);
  prefs.putInt("dpi", cfg.dpi);
  prefs.putInt("labelW", cfg.labelWidth);
  prefs.putInt("labelL", cfg.labelLength);
  prefs.putBool("zplCompr", cfg.zplCompressionSupported);
  prefs.putBool("dbgOut", cfg.debugOutput);
  prefs.putString("dbgMode", cfg.debugOutputMode);
  prefs.putInt("dbgTx", cfg.debugUartTxPin);
  prefs.putInt("dbgRx", cfg.debugUartRxPin);
  prefs.putInt("puTx", cfg.printerUartTxPin);
  prefs.putInt("puRx", cfg.printerUartRxPin);
  prefs.putUInt("puBaud", cfg.printerUartBaud);
  prefs.putUInt("usbBaud", cfg.printerUsbBaud);
  prefs.putInt("qlHeadPx", cfg.qlPrintheadPx);
  prefs.putInt("qlInval", cfg.qlInvalidateBytes);
  prefs.putBool("qlCut", cfg.qlAutoCut);
  prefs.putInt("qlFeed", cfg.qlFeedMarginDots);
  prefs.putInt("qlRMarg", cfg.qlRightMarginDots);
  prefs.putString("ledMode", cfg.ledMode);
  prefs.putInt("ledPin", cfg.ledPin);
  prefs.putInt("ledPinR", cfg.ledPinR);
  prefs.putInt("ledPinG", cfg.ledPinG);
  prefs.putInt("ledPinB", cfg.ledPinB);
  prefs.putString("ledOrder", cfg.ledOrder);
  prefs.putUShort("ledBlink", cfg.ledBlinkMs);
  prefs.putUChar("logLvl", (uint8_t)cfg.logLevel);
  prefs.end();
}

void printRuntimeSettings(const char* reason) {
  dbgPrintln();
  dbgPrintln("================ Stikka ESP32 Settings ================");
  dbgPrint("Reason: ");
  dbgPrintln(reason);
  dbgPrint("Printer name: ");
  dbgPrintln(cfg.printerName);
  dbgPrint("Printer type: ");
  dbgPrintln(PRINTER_TYPE);
  dbgPrint("Location: ");
  dbgPrintln(cfg.location.isEmpty() ? String("<not set>") : cfg.location);
#if defined(TARGET_NETWORK)
  dbgPrint("Target (network): ");
  dbgPrint(cfg.zplTargetHost);
  dbgPrint(":");
  dbgPrintln(cfg.zplTargetPort);
#elif defined(TARGET_SERIAL)
  dbgPrint("Target (serial UART) TX/RX/baud: ");
  dbgPrint(cfg.printerUartTxPin);
  dbgPrint("/");
  dbgPrint(cfg.printerUartRxPin);
  dbgPrint("/");
  dbgPrintln(cfg.printerUartBaud);
#elif defined(TARGET_USB)
  dbgPrint("Target (usb) baud: ");
  dbgPrintln(cfg.printerUsbBaud);
#elif defined(TARGET_USB_HOST)
  dbgPrintln("Target (usb_host): auto-enumerates connected USB printer");
#endif
#ifdef PROTOCOL_QL
  dbgPrint("QL printhead px / invalidate bytes: ");
  dbgPrint(cfg.qlPrintheadPx);
  dbgPrint(" / ");
  dbgPrintln(cfg.qlInvalidateBytes);
  dbgPrint("QL auto-cut / feed margin / right margin: ");
  dbgPrint(cfg.qlAutoCut ? "on" : "off");
  dbgPrint(" / ");
  dbgPrint(cfg.qlFeedMarginDots);
  dbgPrint(" / ");
  dbgPrintln(cfg.qlRightMarginDots);
#endif
  dbgPrint("Label (mm): ");
  dbgPrint(cfg.labelWidth);
  dbgPrint("x");
  dbgPrint(cfg.labelLength);
  dbgPrint(" @ ");
  dbgPrint(cfg.dpi);
  dbgPrintln(" DPI");
  dbgPrint("ZPL compression (:Z64:/:B64:) supported: ");
  dbgPrintln(cfg.zplCompressionSupported ? "yes" : "no");
  dbgPrint("Wi-Fi SSID: ");
  dbgPrintln(cfg.wifiSsid.isEmpty() ? String("<not configured>") : cfg.wifiSsid);
  dbgPrint("Wi-Fi password set: ");
  dbgPrintln(cfg.wifiPassword.isEmpty() ? "no" : "yes");
  dbgPrint("MQTT broker: ");
  if (cfg.mqttHost.isEmpty()) {
    dbgPrintln("<not configured>");
  } else {
    dbgPrint(cfg.mqttHost);
    dbgPrint(":");
    dbgPrintln(cfg.mqttPort);
  }
  dbgPrint("MQTT user: ");
  dbgPrintln(cfg.mqttUser.isEmpty() ? String("<none>") : cfg.mqttUser);
  dbgPrint("MQTT TLS: ");
  dbgPrintln(cfg.mqttUseTls ? "enabled" : "disabled");
  dbgPrint("MQTT TLS insecure: ");
  dbgPrintln(cfg.mqttTlsInsecure ? "enabled" : "disabled");
  dbgPrint("MQTT CA cert: ");
  dbgPrintln(cfg.mqttCaCert.isEmpty() ? "<none>" : "configured");
  dbgPrint("Command topic: ");
  dbgPrintln(commandTopic());
  dbgPrint("Status topic: ");
  dbgPrintln(statusTopic());
  dbgPrint("Debug output: ");
  dbgPrintln(cfg.debugOutput ? "enabled" : "disabled");
  dbgPrint("Debug mode: ");
  dbgPrintln(cfg.debugOutputMode);
  dbgPrint("Debug UART TX/RX: ");
  dbgPrint(cfg.debugUartTxPin);
  dbgPrint("/");
  dbgPrintln(cfg.debugUartRxPin);
  dbgPrint("LED mode: ");
  dbgPrintln(cfg.ledMode);
  dbgPrint("LED order: ");
  dbgPrintln(cfg.ledOrder);
  dbgPrint("LED blink ms: ");
  dbgPrintln(cfg.ledBlinkMs);
  dbgPrintln("========================================================");
  dbgPrintln();
}
