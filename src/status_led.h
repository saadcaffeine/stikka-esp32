#pragma once

enum class LedEventType {
  none,
  rx,
  tx,
};

void setupStatusLed();

// Advances blink phase and paints the LED for the current state. Colors:
// green=WiFi+MQTT, yellow=WiFi only, red=no WiFi/AP, purple=MQTT RX (recent
// markLedEvent(rx)), cyan=MQTT TX (recent markLedEvent(tx)).
void updateStatusLed(bool wifiConnected, bool mqttConnected);

// Flags a brief RX/TX event so updateStatusLed() shows it (purple/cyan)
// instead of the steady-state color for the next couple of blink cycles.
void markLedEvent(LedEventType eventType);
