#include "target.h"

#include "../config.h"

// Runtime dispatch to whichever method cfg.printerMethod selects. Critical
// invariant: usbHostTargetLoop() pumps the USB host library's event queues
// via handles that only exist once usbHostTargetSetup() has run -- calling
// it unconditionally (rather than gated on cfg.printerMethod=="usb_host",
// same as targetSetup() below) would touch an uninitialized clientHandle
// whenever a different method is selected.
void targetSetup() {
  if (cfg.printerMethod == "network") {
    networkTargetSetup();
  } else if (cfg.printerMethod == "serial") {
    serialTargetSetup();
  } else if (cfg.printerMethod == "usb_host") {
    usbHostTargetSetup();
  }
  // else: setup wizard not complete yet, nothing to start.
}

void targetLoop() {
  if (cfg.printerMethod == "usb_host") {
    usbHostTargetLoop();
  }
  // network/serial have nothing to pump between jobs.
}

bool targetSend(const uint8_t* data, size_t len, String& err) {
  if (cfg.printerMethod == "network") return networkTargetSend(data, len, err);
  if (cfg.printerMethod == "serial") return serialTargetSend(data, len, err);
  if (cfg.printerMethod == "usb_host") return usbHostTargetSend(data, len, err);
  err = "printer connection method not configured yet (finish setup wizard)";
  return false;
}

bool targetSendString(const String& body, String& err) {
  if (cfg.printerMethod == "network") return networkTargetSendString(body, err);
  if (cfg.printerMethod == "serial") return serialTargetSendString(body, err);
  if (cfg.printerMethod == "usb_host") return usbHostTargetSendString(body, err);
  err = "printer connection method not configured yet (finish setup wizard)";
  return false;
}

// network/serial: no incremental API, so the whole (already time-bounded)
// blocking send runs inside the first poll.
static const uint8_t* blockingData = nullptr;
static size_t blockingLen = 0;
static bool blockingPending = false;

bool targetSendBegin(const uint8_t* data, size_t len, String& err) {
  if (cfg.printerMethod == "usb_host") return usbHostTargetSendBegin(data, len, err);
  if (cfg.printerMethod != "network" && cfg.printerMethod != "serial") {
    err = "printer connection method not configured yet (finish setup wizard)";
    return false;
  }
  blockingData = data;
  blockingLen = len;
  blockingPending = true;
  return true;
}

TargetSendState targetSendPoll(String& err) {
  if (cfg.printerMethod == "usb_host") return usbHostTargetSendPoll(err);
  if (!blockingPending) return TargetSendState::Done;
  blockingPending = false;
  return targetSend(blockingData, blockingLen, err) ? TargetSendState::Done : TargetSendState::Failed;
}

void targetSendProgress(size_t& sent, size_t& total) {
  if (cfg.printerMethod == "usb_host") {
    usbHostTargetSendProgress(sent, total);
    return;
  }
  sent = blockingPending ? 0 : blockingLen;
  total = blockingLen;
}

bool targetNeedsReboot() {
  return cfg.printerMethod == "usb_host" && usbHostTargetNeedsReboot();
}

const char* targetMethodName() {
  // cfg is a persistent global, so cfg.printerMethod.c_str() is stable here
  // -- deliberately not a ternary mixing a String temporary with a literal,
  // which would return a dangling pointer once that temporary is destroyed.
  if (cfg.printerMethod.isEmpty()) return "unconfigured";
  return cfg.printerMethod.c_str();
}

bool writeStreamWithTimeout(Print& out, const uint8_t* data, size_t len, size_t chunkSize, String& err) {
  size_t sent = 0;
  const unsigned long startedAt = millis();
  unsigned long lastProgressAt = startedAt;
  const unsigned long idleTimeoutMs = 7000;
  const unsigned long totalTimeoutMs = 60000;

  while (sent < len) {
    const unsigned long now = millis();
    if (now - startedAt > totalTimeoutMs) {
      err = "write loop stop: total timeout";
      break;
    }
    if (now - lastProgressAt > idleTimeoutMs) {
      err = "write loop stop: idle timeout";
      break;
    }

    const size_t remaining = len - sent;
    const size_t toWrite = remaining > chunkSize ? chunkSize : remaining;
    const size_t written = out.write(data + sent, toWrite);
    if (written == 0) {
      delay(2);
      continue;
    }
    sent += written;
    lastProgressAt = millis();
  }

  if (sent != len) {
    if (err.isEmpty()) err = "partial send " + String(sent) + "/" + String(len) + " bytes";
    return false;
  }
  return true;
}
