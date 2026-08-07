#include "target.h"

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
