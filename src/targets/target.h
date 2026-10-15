#pragma once

#include <Arduino.h>

// One shared contract each transport method implements. Unlike the old
// compile-time-select-one-method firmware, network_target.cpp/
// serial_target.cpp/usb_host_target.cpp are ALL always compiled in now, each
// under its own unique function names (networkTarget*/serialTarget*/
// usbHostTarget*, declared below) -- target.cpp's dispatcher implements the
// functions in this header by switching on cfg.printerMethod at runtime and
// calling into whichever one is selected. mqtt_bridge.cpp and web_ui.cpp
// call these dispatcher functions without knowing which method is active.

// One-shot: build a complete buffer, then transmit it. Used by the zpl/image
// passthrough path and the web UI's test-print button.
bool targetSend(const uint8_t* data, size_t len, String& err);
bool targetSendString(const String& body, String& err);

// Non-blocking send, driven by print_queue.cpp from loop(): Begin once, then
// Poll every loop() pass until it returns Done or Failed (err set). `data`
// must stay valid until then. The USB host method really is incremental
// (one bulk transfer per poll, with per-chunk/idle/total timeouts); network
// and serial do their existing bounded blocking send inside the first Poll.
enum class TargetSendState : uint8_t { Running, Done, Failed };
bool targetSendBegin(const uint8_t* data, size_t len, String& err);
TargetSendState targetSendPoll(String& err);
void targetSendProgress(size_t& sent, size_t& total);
// True when the transport is wedged beyond recovery and only a reboot helps.
bool targetNeedsReboot();

// Human-readable transport identifier, mirrors cfg.printerMethod ("network"/
// "serial"/"usb_host", or "unconfigured" before the setup wizard is done) --
// used by the web UI and boot log.
const char* targetMethodName();

// Called once from setup(), after loadConfig() -- dispatches to whichever
// method's *TargetSetup() is currently selected (letting the serial target
// open its UART at the configured baud before the first job arrives, or the
// usb_host target install the USB host stack). No-op if the setup wizard
// hasn't chosen a method yet, or for the network target (it opens a fresh
// TCP connection per job/stream instead).
void targetSetup();

// Called every loop() iteration. Only dispatches to usb_host's *TargetLoop()
// (pumping the USB host library's event queues, which must happen
// continuously for enumeration/transfers to make progress) when usb_host is
// the selected method -- every other method has nothing to pump.
void targetLoop();

// Shared byte-by-byte write loop with idle/total timeouts, for any Arduino
// Print-derived transport that's already open (HardwareSerial, Serial).
// The network target doesn't use this -- it also needs to notice the TCP
// socket dropping mid-transfer, which isn't expressible via a generic
// Print&, so it keeps its own loop.
bool writeStreamWithTimeout(Print& out, const uint8_t* data, size_t len, size_t chunkSize, String& err);

// Per-method implementations -- network_target.cpp, serial_target.cpp,
// usb_host_target.cpp. Always compiled in; only called by target.cpp's
// dispatcher when cfg.printerMethod selects that method. Not intended to be
// called directly from anywhere else.
void networkTargetSetup();
void networkTargetLoop();
bool networkTargetSend(const uint8_t* data, size_t len, String& err);
bool networkTargetSendString(const String& body, String& err);

void serialTargetSetup();
void serialTargetLoop();
bool serialTargetSend(const uint8_t* data, size_t len, String& err);
bool serialTargetSendString(const String& body, String& err);

void usbHostTargetSetup();
void usbHostTargetLoop();
bool usbHostTargetSend(const uint8_t* data, size_t len, String& err);
bool usbHostTargetSendString(const String& body, String& err);
bool usbHostTargetSendBegin(const uint8_t* data, size_t len, String& err);
TargetSendState usbHostTargetSendPoll(String& err);
void usbHostTargetSendProgress(size_t& sent, size_t& total);
bool usbHostTargetNeedsReboot();

// True once a USB printer has been enumerated and its bulk-OUT endpoint claimed
// (used by the web UI status pill). Always false for non-usb_host setups.
bool usbHostTargetReady();
