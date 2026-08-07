#pragma once

#include <Arduino.h>

#include "config.h"

// In-memory log ring buffer backing the web UI's Logs tab. dbgPrint()/
// dbgPrintln() assemble a line from several print() calls before the
// trailing dbgPrintln() flushes it -- flushPendingLogLine() is what
// actually pushes one assembled line into the ring buffer (and, if enabled,
// out to serial/UART), so the buffer holds the same one-line-per-message
// shape as the old serial output instead of one entry per print() fragment.
class LineCapture : public Print {
 public:
  explicit LineCapture(String* t) : target(t) {}
  size_t write(uint8_t c) override {
    *target += (char)c;
    return 1;
  }
  size_t write(const uint8_t* buffer, size_t size) override {
    target->concat(reinterpret_cast<const char*>(buffer), size);
    return size;
  }

 private:
  String* target;
};

extern bool debugOutputEnabled;
extern String pendingLogLine;
extern LogLevel pendingLogLevel;
extern LineCapture lineCapture;

void applyDebugOutputSetting(bool enabled);
void flushPendingLogLine();
String shortenForLog(const String& text, size_t maxLen = 160);
void dbgPrintHeadTailBytes(const uint8_t* data, size_t len, size_t n = 300);
void dbgPrintHeadTail(const String& s, size_t n = 300);

// Serializes ring-buffer entries newer than sinceSeq (up to `limit`) as the
// JSON payload the web Logs tab's poll() expects -- see logEntriesToJson()
// in logging.cpp for the paging rationale.
String logEntriesToJson(uint32_t sinceSeq, size_t limit = 40);
void logBufferClear();

// Takes the most severe (lowest-numbered) level seen across a chain rather
// than simply the last call's level -- today every chain only ever passes an
// explicit non-default level on its trailing call, so this changes nothing
// yet, but it means a future fragment added after an elevated trailing call
// can't silently downgrade it.
inline void raiseLogLevel(LogLevel level) {
  if ((uint8_t)level < (uint8_t)pendingLogLevel) pendingLogLevel = level;
}

template <typename T>
inline void dbgPrint(const T& value, LogLevel level = LogLevel::LOG_DEBUG) {
  raiseLogLevel(level);
  lineCapture.print(value);
}

template <typename T>
inline void dbgPrintln(const T& value, LogLevel level = LogLevel::LOG_DEBUG) {
  raiseLogLevel(level);
  lineCapture.print(value);
  flushPendingLogLine();
}

inline void dbgPrintln(LogLevel level = LogLevel::LOG_DEBUG) {
  raiseLogLevel(level);
  flushPendingLogLine();
}
