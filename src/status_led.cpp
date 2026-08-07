#include "status_led.h"

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

#include "config.h"
#include "logging.h"

struct RgbColor {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

static Adafruit_NeoPixel* statusPixel = nullptr;
static bool ledConfigured = false;
static unsigned long ledLastToggleMs = 0;
static bool ledBlinkOn = false;
static unsigned long ledEventUntilMs = 0;
static LedEventType ledEventType = LedEventType::none;

static neoPixelType neopixelTypeFromOrder(const String& orderRaw) {
  String order = orderRaw;
  order.toUpperCase();
  if (order == "RGB") return NEO_RGB + NEO_KHZ800;
  if (order == "RBG") return NEO_RBG + NEO_KHZ800;
  if (order == "GRB") return NEO_GRB + NEO_KHZ800;
  if (order == "GBR") return NEO_GBR + NEO_KHZ800;
  if (order == "BRG") return NEO_BRG + NEO_KHZ800;
  if (order == "BGR") return NEO_BGR + NEO_KHZ800;
  return NEO_GRB + NEO_KHZ800;
}

static uint8_t componentByOrder(char channel, const RgbColor& c) {
  switch (channel) {
    case 'R': return c.r;
    case 'G': return c.g;
    case 'B': return c.b;
    default: return 0;
  }
}

static void clearStatusLed() {
  if (cfg.ledMode == "neopixel") {
    if (statusPixel) {
      statusPixel->setPixelColor(0, statusPixel->Color(0, 0, 0));
      statusPixel->show();
    }
    return;
  }

  if (cfg.ledMode == "rgb") {
    if (cfg.ledPinR >= 0) digitalWrite(cfg.ledPinR, LOW);
    if (cfg.ledPinG >= 0) digitalWrite(cfg.ledPinG, LOW);
    if (cfg.ledPinB >= 0) digitalWrite(cfg.ledPinB, LOW);
  }
}

void setupStatusLed() {
  ledConfigured = false;
  ledBlinkOn = false;
  ledEventUntilMs = 0;
  ledEventType = LedEventType::none;

  if (statusPixel) {
    statusPixel->clear();
    statusPixel->show();
    delete statusPixel;
    statusPixel = nullptr;
  }

  if (cfg.ledBlinkMs < 100) cfg.ledBlinkMs = 100;
  if (cfg.ledBlinkMs > 5000) cfg.ledBlinkMs = 5000;

  if (cfg.ledMode == "neopixel") {
    if (cfg.ledPin < 0) {
      dbgPrintln("[led] neopixel mode selected but ledPin < 0", LogLevel::LOG_WARN);
      return;
    }
    statusPixel = new Adafruit_NeoPixel(1, (uint8_t)cfg.ledPin, neopixelTypeFromOrder(cfg.ledOrder));
    statusPixel->begin();
    statusPixel->clear();
    statusPixel->show();
    ledConfigured = true;
    dbgPrint("[led] neopixel on pin ");
    dbgPrint(cfg.ledPin);
    dbgPrint(", order=");
    dbgPrintln(cfg.ledOrder, LogLevel::LOG_INFO);
    return;
  }

  if (cfg.ledMode == "rgb") {
    if (cfg.ledPinR < 0 || cfg.ledPinG < 0 || cfg.ledPinB < 0) {
      dbgPrintln("[led] rgb mode selected but one or more RGB pins are invalid", LogLevel::LOG_WARN);
      return;
    }
    pinMode((uint8_t)cfg.ledPinR, OUTPUT);
    pinMode((uint8_t)cfg.ledPinG, OUTPUT);
    pinMode((uint8_t)cfg.ledPinB, OUTPUT);
    clearStatusLed();
    ledConfigured = true;
    dbgPrint("[led] rgb pins R/G/B = ");
    dbgPrint(cfg.ledPinR);
    dbgPrint("/");
    dbgPrint(cfg.ledPinG);
    dbgPrint("/");
    dbgPrintln(cfg.ledPinB, LogLevel::LOG_INFO);
    return;
  }

  dbgPrintln("[led] status LED disabled", LogLevel::LOG_INFO);
}

static RgbColor baseStatusColor(bool wifiConnected, bool mqttConnected) {
  if (!wifiConnected) {
    return {255, 0, 0};      // red: fallback AP / no Wi-Fi
  }
  if (!mqttConnected) {
    return {255, 180, 0};    // yellow: Wi-Fi yes, MQTT no
  }
  return {0, 255, 0};        // green: Wi-Fi + MQTT
}

static RgbColor eventStatusColor(LedEventType evt, bool wifiConnected, bool mqttConnected) {
  if (evt == LedEventType::rx) {
    return {180, 0, 255};    // purple: receiving over MQTT
  }
  if (evt == LedEventType::tx) {
    return {0, 255, 255};    // cyan: sending over MQTT
  }
  return baseStatusColor(wifiConnected, mqttConnected);
}

static void setLedColor(const RgbColor& c) {
  if (!ledConfigured) return;

  if (cfg.ledMode == "neopixel") {
    if (!statusPixel) return;
    statusPixel->setPixelColor(0, statusPixel->Color(c.r, c.g, c.b));
    statusPixel->show();
    return;
  }

  if (cfg.ledMode == "rgb") {
    String order = cfg.ledOrder;
    order.toUpperCase();
    const char c0 = order.length() > 0 ? order[0] : 'R';
    const char c1 = order.length() > 1 ? order[1] : 'G';
    const char c2 = order.length() > 2 ? order[2] : 'B';
    const uint8_t vR = componentByOrder(c0, c);
    const uint8_t vG = componentByOrder(c1, c);
    const uint8_t vB = componentByOrder(c2, c);
    digitalWrite((uint8_t)cfg.ledPinR, vR > 0 ? HIGH : LOW);
    digitalWrite((uint8_t)cfg.ledPinG, vG > 0 ? HIGH : LOW);
    digitalWrite((uint8_t)cfg.ledPinB, vB > 0 ? HIGH : LOW);
  }
}

void markLedEvent(LedEventType eventType) {
  if (!ledConfigured) return;
  ledEventType = eventType;
  const unsigned long now = millis();
  const unsigned long holdMs = (unsigned long)cfg.ledBlinkMs * 2UL;
  ledEventUntilMs = now + (holdMs < 300 ? 300 : holdMs);
}

void updateStatusLed(bool wifiConnected, bool mqttConnected) {
  if (!ledConfigured) return;
  const unsigned long now = millis();
  const unsigned long halfBlink = (unsigned long)cfg.ledBlinkMs / 2UL;
  if (halfBlink == 0) return;

  if (now - ledLastToggleMs >= halfBlink) {
    ledLastToggleMs = now;
    ledBlinkOn = !ledBlinkOn;
  }

  LedEventType activeEvent = LedEventType::none;
  if (ledEventUntilMs > now) {
    activeEvent = ledEventType;
  }

  if (!ledBlinkOn) {
    setLedColor({0, 0, 0});
    return;
  }

  setLedColor(eventStatusColor(activeEvent, wifiConnected, mqttConnected));
}
