#include "target.h"

#ifdef TARGET_NETWORK

#include <WiFi.h>

#include "../config.h"
#include "../logging.h"

// "network" transport method: relays bytes to a plain TCP printer target
// (cfg.zplTargetHost:cfg.zplTargetPort). Protocol-agnostic passthrough --
// this firmware doesn't parse ZPL/image/QL-raster data itself here, it just
// forwards whatever bytes the caller hands it (the frontend builds any
// protocol-specific byte stream, e.g. ZPL or Brother QL raster, client-side).
static WiFiClient streamClient;

const char* targetMethodName() { return "network"; }

void targetSetup() {
  // Nothing to pre-open -- a fresh TCP connection is made per job/stream.
}

void targetLoop() {
  // Nothing to pump -- WiFiClient's own blocking calls handle everything.
}

bool targetSend(const uint8_t* data, size_t len, String& err) {
  if (len == 0) {
    err = "empty payload";
    return false;
  }

  if (cfg.zplTargetHost.isEmpty()) {
    err = "zplTargetHost is empty";
    return false;
  }

  if (!streamClient.connect(cfg.zplTargetHost.c_str(), cfg.zplTargetPort)) {
    err = "cannot connect to target printer";
    return false;
  }

  dbgPrint("[target] tcp connected -> ");
  dbgPrint(cfg.zplTargetHost);
  dbgPrint(":");
  dbgPrintln(cfg.zplTargetPort);

  dbgPrint("[target] sending bytes: ");
  dbgPrintln(len);

  size_t sent = 0;
  const unsigned long startedAt = millis();
  unsigned long lastProgressAt = startedAt;
  const unsigned long idleTimeoutMs = 7000;
  const unsigned long totalTimeoutMs = 60000;
  const size_t chunkSize = 1024;

  while (sent < len) {
    const unsigned long now = millis();
    if (!streamClient.connected()) {
      err = "write loop stop: socket disconnected";
      break;
    }
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
    const size_t written = streamClient.write(data + sent, toWrite);
    if (written == 0) {
      delay(2);
      continue;
    }
    sent += written;
    lastProgressAt = millis();
  }

  streamClient.flush();
  streamClient.stop();

  if (sent != len) {
    if (err.isEmpty()) err = "partial send " + String(sent) + "/" + String(len) + " bytes";
    return false;
  }
  return true;
}

bool targetSendString(const String& body, String& err) {
  if (body.length() == 0) {
    err = "empty zpl payload";
    return false;
  }
  return targetSend(reinterpret_cast<const uint8_t*>(body.c_str()), body.length(), err);
}

#endif // TARGET_NETWORK
