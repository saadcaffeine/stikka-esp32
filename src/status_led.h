#pragma once

enum class LedEventType {
  none,
  rx,
  tx,
};

// LED wiring (mode/pin/order/blink speed) is a compile-time-only property of
// the board (see DEFAULT_LED_* build_flags in platformio.ini) -- there's
// only one board/LED combination today, so it isn't NVS-backed or
// web-UI-editable. setupStatusLed() is called once at boot only.
void setupStatusLed();

// Advances blink phase and paints the LED for the current state. Colors:
// green=WiFi+MQTT, yellow=WiFi only, red=no WiFi/AP, purple=MQTT RX (recent
// markLedEvent(rx)), cyan=MQTT TX (recent markLedEvent(tx)).
void updateStatusLed(bool wifiConnected, bool mqttConnected);

// Flags a brief RX/TX event so updateStatusLed() shows it (purple/cyan)
// instead of the steady-state color for the next couple of blink cycles.
void markLedEvent(LedEventType eventType);
