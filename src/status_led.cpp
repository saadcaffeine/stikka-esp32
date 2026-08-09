#include "status_led.h"

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

#include "config.h"
#include "logging.h"

// Compile-time-only LED wiring -- see platformio.ini's per-board build_flags.
// Fallback defaults here let this file build standalone even without them.
#ifndef DEFAULT_LED_MODE
#define DEFAULT_LED_MODE "none"
#endif
#ifndef DEFAULT_LED_PIN
#define DEFAULT_LED_PIN -1
#endif
#ifndef DEFAULT_LED_ORDER
#define DEFAULT_LED_ORDER "GRB"
#endif
#ifndef DEFAULT_LED_BLINK_MS
#define DEFAULT_LED_BLINK_MS 700
#endif
// No board uses discrete-RGB mode today, so unlike LED_MODE/PIN/ORDER these
// have never been exposed as build_flags -- add DEFAULT_LED_PIN_R/G/B if a
// future board needs "rgb" mode.
#define LED_PIN_R -1
#define LED_PIN_G -1
#define LED_PIN_B -1

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
static uint16_t ledBlinkMs = DEFAULT_LED_BLINK_MS;

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
  if (String(DEFAULT_LED_MODE) == "neopixel") {
    if (statusPixel) {
      statusPixel->setPixelColor(0, statusPixel->Color(0, 0, 0));
      statusPixel->show();
    }
    return;
  }

  if (String(DEFAULT_LED_MODE) == "rgb") {
    if (LED_PIN_R >= 0) digitalWrite(LED_PIN_R, LOW);
    if (LED_PIN_G >= 0) digitalWrite(LED_PIN_G, LOW);
    if (LED_PIN_B >= 0) digitalWrite(LED_PIN_B, LOW);
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

  ledBlinkMs = DEFAULT_LED_BLINK_MS;
  if (ledBlinkMs < 100) ledBlinkMs = 100;
  if (ledBlinkMs > 5000) ledBlinkMs = 5000;

  if (String(DEFAULT_LED_MODE) == "neopixel") {
    if (DEFAULT_LED_PIN < 0) {
      dbgPrintln("[led] neopixel mode selected but DEFAULT_LED_PIN < 0", LogLevel::LOG_WARN);
      return;
    }
    statusPixel = new Adafruit_NeoPixel(1, (uint8_t)DEFAULT_LED_PIN, neopixelTypeFromOrder(DEFAULT_LED_ORDER));
    statusPixel->begin();
    statusPixel->clear();
    statusPixel->show();
    statusPixel->setBrightness(25);
    ledConfigured = true;
    dbgPrint("[led] neopixel on pin ");
    dbgPrint(DEFAULT_LED_PIN);
    dbgPrint(", order=");
    dbgPrintln(DEFAULT_LED_ORDER, LogLevel::LOG_INFO);
    return;
  }

  if (String(DEFAULT_LED_MODE) == "rgb") {
    if (LED_PIN_R < 0 || LED_PIN_G < 0 || LED_PIN_B < 0) {
      dbgPrintln("[led] rgb mode selected but one or more RGB pins are invalid", LogLevel::LOG_WARN);
      return;
    }
    pinMode((uint8_t)LED_PIN_R, OUTPUT);
    pinMode((uint8_t)LED_PIN_G, OUTPUT);
    pinMode((uint8_t)LED_PIN_B, OUTPUT);
    clearStatusLed();
    ledConfigured = true;
    dbgPrint("[led] rgb pins R/G/B = ");
    dbgPrint(LED_PIN_R);
    dbgPrint("/");
    dbgPrint(LED_PIN_G);
    dbgPrint("/");
    dbgPrintln(LED_PIN_B, LogLevel::LOG_INFO);
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

  if (String(DEFAULT_LED_MODE) == "neopixel") {
    if (!statusPixel) return;
    statusPixel->setPixelColor(0, statusPixel->Color(c.r, c.g, c.b));
    statusPixel->show();
    return;
  }

  if (String(DEFAULT_LED_MODE) == "rgb") {
    String order = DEFAULT_LED_ORDER;
    order.toUpperCase();
    const char c0 = order.length() > 0 ? order[0] : 'R';
    const char c1 = order.length() > 1 ? order[1] : 'G';
    const char c2 = order.length() > 2 ? order[2] : 'B';
    const uint8_t vR = componentByOrder(c0, c);
    const uint8_t vG = componentByOrder(c1, c);
    const uint8_t vB = componentByOrder(c2, c);
    digitalWrite((uint8_t)LED_PIN_R, vR > 0 ? HIGH : LOW);
    digitalWrite((uint8_t)LED_PIN_G, vG > 0 ? HIGH : LOW);
    digitalWrite((uint8_t)LED_PIN_B, vB > 0 ? HIGH : LOW);
  }
}

void markLedEvent(LedEventType eventType) {
  if (!ledConfigured) return;
  ledEventType = eventType;
  const unsigned long now = millis();
  const unsigned long holdMs = (unsigned long)ledBlinkMs * 2UL;
  ledEventUntilMs = now + (holdMs < 300 ? 300 : holdMs);
}

void updateStatusLed(bool wifiConnected, bool mqttConnected) {
  if (!ledConfigured) return;
  const unsigned long now = millis();
  const unsigned long halfBlink = (unsigned long)ledBlinkMs / 2UL;
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
