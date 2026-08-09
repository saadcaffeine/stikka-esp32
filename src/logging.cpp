#include "logging.h"

#include <ArduinoJson.h>

#include <cstring>

bool debugOutputEnabled = true;
String pendingLogLine;
LogLevel pendingLogLevel = LogLevel::LOG_DEBUG;
LineCapture lineCapture(&pendingLogLine);

static bool debugUsbActive = false;
static Print* debugOut = nullptr;

// Fixed-size line storage (matching shortenForLog()'s own truncation length)
// instead of String: 120 entries reassigned forever at runtime would mean a
// continuous heap alloc/realloc cycle, exactly what this file's MQTT-buffer
// comments elsewhere say to avoid. A static array costs a fixed ~20KB of BSS
// instead, which every board in platformio.ini (>=320KB SRAM) has to spare.
static const size_t LOG_LINE_MAX = 160;

struct LogEntry {
  uint32_t seq = 0;
  uint32_t ms = 0;
  LogLevel level = LogLevel::LOG_DEBUG;
  char line[LOG_LINE_MAX] = {0};
};

static const size_t LOG_BUFFER_CAPACITY = 120;
static LogEntry logBuffer[LOG_BUFFER_CAPACITY];
static size_t logBufferNext = 0;
static size_t logBufferCount = 0;
static uint32_t logSeqCounter = 0;

static void pushLogEntry(LogLevel level, const String& line) {
  LogEntry& e = logBuffer[logBufferNext];
  e.seq = ++logSeqCounter;
  e.ms = millis();
  e.level = level;
  size_t n = line.length();
  if (n > LOG_LINE_MAX - 1) n = LOG_LINE_MAX - 1;
  memcpy(e.line, line.c_str(), n);
  e.line[n] = '\0';
  logBufferNext = (logBufferNext + 1) % LOG_BUFFER_CAPACITY;
  if (logBufferCount < LOG_BUFFER_CAPACITY) logBufferCount++;
}

void logBufferClear() {
  logBufferCount = 0;
  logBufferNext = 0;
}

// Walks the ring buffer oldest-to-newest and serializes entries newer than
// `sinceSeq` -- the web UI polls this repeatedly, only asking for what it
// hasn't already appended to the page. Capped at `limit` per call: a first
// page load (since=0) or a client that fell behind could otherwise ask for
// all 120 buffered lines in one JsonDocument at once. A client that's still
// behind just catches up over the next couple of polls, since it always
// advances its own `since` to the newest seq it received.
String logEntriesToJson(uint32_t sinceSeq, size_t limit) {
  JsonDocument doc;
  doc["logLevel"] = logLevelName(cfg.logLevel);
  doc["uptimeMs"] = millis();
  JsonArray arr = doc["entries"].to<JsonArray>();

  const size_t count = logBufferCount;
  const size_t startIdx = (count < LOG_BUFFER_CAPACITY) ? 0 : logBufferNext;
  size_t added = 0;
  for (size_t i = 0; i < count && added < limit; i++) {
    const LogEntry& e = logBuffer[(startIdx + i) % LOG_BUFFER_CAPACITY];
    if (e.seq <= sinceSeq) continue;
    JsonObject o = arr.add<JsonObject>();
    o["seq"] = e.seq;
    o["ms"] = e.ms;
    o["level"] = logLevelName(e.level);
    o["msg"] = e.line;
    added++;
  }

  String out;
  serializeJson(doc, out);
  return out;
}

// Flushes whatever dbgPrint() fragments have accumulated in pendingLogLine as
// one log line at `level`. Lines above the configured verbosity (cfg.logLevel)
// are dropped entirely -- from both the serial/UART sink and the ring buffer,
// so log level is a single knob for both. Below that threshold, the line
// always goes into the ring buffer (so the web Logs tab works even with
// serial output disabled) and additionally goes to debugOut when serial
// output is separately enabled via cfg.debugOutput.
//
// Trade-off vs. the old per-fragment-writes-immediately behavior: nothing
// reaches serial until the chain's trailing dbgPrintln() flushes it, so a
// crash/watchdog reset between the first dbgPrint() and that flush now loses
// the whole in-progress line instead of whatever fragments had already
// printed. Every call chain in this file is a short run of String-only
// fragments with no blocking/network call in between, so the window is
// narrow -- accepted here in exchange for one-line-per-message log entries.
void flushPendingLogLine() {
  const LogLevel level = pendingLogLevel;
  const String line = pendingLogLine;
  pendingLogLine = "";
  pendingLogLevel = LogLevel::LOG_DEBUG;
  if ((uint8_t)level > (uint8_t)cfg.logLevel) return;
  if (debugOutputEnabled && debugOut != nullptr) {
    debugOut->println(line);
  }
  if (line.length() > 0) pushLogEntry(level, line);
}

String shortenForLog(const String& text, size_t maxLen) {
  if (text.length() <= maxLen) return text;
  return text.substring(0, maxLen) + "...";
}

static void stopDebugTransport() {
  if (debugUsbActive) {
    Serial.flush();
    Serial.end();
    debugUsbActive = false;
  }
  debugOut = nullptr;
}

// Debug output always goes out Serial, which always resolves to real UART0
// (the board's programming-bridge port) since ARDUINO_USB_CDC_ON_BOOT=0 is
// unconditional now (see platformio.ini) -- that port is physically
// independent from the native USB-OTG connector the usb_host printer method
// uses, so debug logging and usb_host printing never conflict regardless of
// which printer method is runtime-selected.
void applyDebugOutputSetting(bool enabled) {
  debugOutputEnabled = enabled;
  stopDebugTransport();
  if (!debugOutputEnabled) return;

  Serial.begin(115200);
  delay(50);
  debugUsbActive = true;
  debugOut = &Serial;
  dbgPrintln("[debug] serial logging enabled (UART0 / programming port)", LogLevel::LOG_INFO);
}

// Printing tens of KB in one blocking println() can starve the WiFi/MQTT
// background tasks long enough to trip the watchdog, and most serial
// monitors truncate output that long anyway. Head+tail is enough to confirm
// the body wasn't cut off (in particular, that it still ends in ^FS/^XZ).
// The length/head/tail labels go through dbgPrint/dbgPrintln like any other
// log line (so they still reach the web Logs tab even with serial output
// off). The full head+tail dump stays serial-only -- up to 600 bytes across
// two lines is more than one ring-buffer entry (LOG_LINE_MAX) can hold and
// ZPL bodies can run to tens of KB (e.g. embedded ^GF compressed graphics),
// so instead a short, explicitly bounded preview of the start of the body
// (ZPL is plain ASCII text, so this is always safe to show) goes through
// dbgPrint/lineCapture -- capped up front so pendingLogLine never grows by
// more than a couple hundred bytes here regardless of the real body size.
void dbgPrintHeadTailBytes(const uint8_t* data, size_t len, size_t n) {
  dbgPrint("[zpl] body length=");
  dbgPrintln((unsigned long)len);

  static const size_t kPreviewMax = LOG_LINE_MAX > 32 ? LOG_LINE_MAX - 32 : 32;
  const size_t previewLen = len < kPreviewMax ? len : kPreviewMax;
  dbgPrint("[zpl] body: ");
  lineCapture.write(data, previewLen);
  if (previewLen < len) dbgPrint("...");
  dbgPrintln();

  const bool rawOut = debugOutputEnabled && debugOut != nullptr;
  if (len <= n * 2) {
    if (rawOut) {
      debugOut->write(data, len);
      debugOut->println();
    }
    return;
  }
  dbgPrintln("[zpl] head:");
  if (rawOut) {
    debugOut->write(data, n);
    debugOut->println();
  }
  dbgPrintln("[zpl] tail:");
  if (rawOut) {
    debugOut->write(data + (len - n), n);
    debugOut->println();
  }
}

void dbgPrintHeadTail(const String& s, size_t n) {
  dbgPrintHeadTailBytes(reinterpret_cast<const uint8_t*>(s.c_str()), s.length(), n);
}
