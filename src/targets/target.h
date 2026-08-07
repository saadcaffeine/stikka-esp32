#pragma once

#include <Arduino.h>

// One shared contract each compiled-in transport method (network/serial/usb)
// implements -- exactly one targets/*_target.cpp provides these symbols,
// selected by which TARGET_NETWORK / TARGET_SERIAL / TARGET_USB build flag
// the active platformio.ini env defines (see platformio.ini's env_* base
// sections). mqtt_bridge.cpp and web_ui.cpp call these without knowing which
// method is actually compiled in.

// One-shot: build a complete buffer, then transmit it. Used by the zpl/image
// passthrough path and the web UI's test-print button.
bool targetSend(const uint8_t* data, size_t len, String& err);
bool targetSendString(const String& body, String& err);

// Human-readable transport identifier ("network"/"serial"/"usb"), matches
// the active env's METHOD build_flag -- used by the web UI and boot log.
const char* targetMethodName();

// Called once from setup(), after loadConfig() -- lets the serial/usb
// targets open their UART at the configured baud before the first job
// arrives. No-op for the network target (it opens a fresh TCP connection
// per job/stream instead).
void targetSetup();

// Called every loop() iteration. Only the usb_host target does anything
// here (pumping the USB host library's event queues, which must happen
// continuously for enumeration/transfers to make progress) -- every other
// target implements this as an empty no-op.
void targetLoop();

// Shared byte-by-byte write loop with idle/total timeouts, for any Arduino
// Print-derived transport that's already open (HardwareSerial, Serial).
// The network target doesn't use this -- it also needs to notice the TCP
// socket dropping mid-transfer, which isn't expressible via a generic
// Print&, so it keeps its own loop.
bool writeStreamWithTimeout(Print& out, const uint8_t* data, size_t len, size_t chunkSize, String& err);
