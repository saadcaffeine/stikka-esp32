#include "print_queue.h"

#include <utility>

#include "logging.h"
#include "mqtt_bridge.h"
#include "targets/target.h"

namespace {

struct PrintJob {
  String id;
  String tag;  // "ql", "zpl", "image", "seiko", "test" -- log prefix only
  std::unique_ptr<uint8_t[]> data;
  size_t len = 0;
  unsigned long queuedMs = 0;
};

// Jobs live in PSRAM-backed heap (decoded bytes, typically 20-80 KB each), so a
// handful is cheap. Beyond this, new jobs are refused instead of piling up
// behind a printer that may be jammed or out of labels.
const size_t kQueueDepth = 4;
// A job that waited this long behind others is dropped rather than printed:
// whoever sent it has most likely given up or re-sent it.
const unsigned long kMaxQueueWaitMs = 120000;

PrintJob queue[kQueueDepth];
size_t head = 0;
size_t count = 0;

bool running = false;
PrintJob current;
unsigned long startedMs = 0;
String lastError;

PrintJob popFront() {
  PrintJob j = std::move(queue[head]);
  head = (head + 1) % kQueueDepth;
  count--;
  return j;
}

void finishCurrent(bool ok, const String& err) {
  running = false;
  const unsigned long ms = millis() - startedMs;
  if (ok) {
    dbgPrintln("[" + current.tag + "] job " + current.id + " done in " + String(ms) + " ms", LogLevel::LOG_INFO);
    publishJobStatus(current.id.c_str(), "done", "sent to printer");
    lastError = "";
  } else {
    dbgPrintln("[" + current.tag + "] job " + current.id + " failed after " + String(ms) + " ms: " + err, LogLevel::LOG_ERROR);
    publishJobStatus(current.id.c_str(), "failed", err.c_str());
    lastError = err;
  }
  current.data.reset();
  current.len = 0;
  if (count > 0) return;  // next job starts right away and publishes "printing" itself
  publishStatus(ok ? "ready" : "error", ok ? "" : err.c_str());
}

void failAllQueued(const String& why) {
  while (count > 0) {
    PrintJob j = popFront();
    publishJobStatus(j.id.c_str(), "failed", why.c_str());
  }
}

}  // namespace

bool printQueueEnqueue(const String& jobId, const String& tag, std::unique_ptr<uint8_t[]> data, size_t len, String& err) {
  if (count >= kQueueDepth) {
    err = "printer busy: " + String(kQueueDepth) + " jobs already waiting, try again shortly";
    return false;
  }
  PrintJob& j = queue[(head + count) % kQueueDepth];
  j.id = jobId;
  j.tag = tag;
  j.data = std::move(data);
  j.len = len;
  j.queuedMs = millis();
  count++;
  const size_t ahead = count - 1 + (running ? 1 : 0);
  dbgPrintln("[queue] " + tag + " job " + jobId + " queued, " + String(len) + " bytes, " + String(ahead) + " ahead", LogLevel::LOG_INFO);
  if (ahead > 0) publishJobStatus(jobId.c_str(), "queued", ("waiting behind " + String(ahead) + " job(s)").c_str());
  return true;
}

void printQueueLoop() {
  if (running) {
    String err;
    const TargetSendState state = targetSendPoll(err);
    if (state == TargetSendState::Running) return;
    finishCurrent(state == TargetSendState::Done, err);
  }

  // The USB host stack can (rarely) end up with a transfer it will never
  // complete or cancel; after that nothing can be sent until a reboot. Fail
  // whatever is waiting with a clear message, then restart. Settings live in
  // NVS, so the board comes back exactly as configured within a few seconds.
  if (targetNeedsReboot()) {
    const String why = "printer USB connection stuck, bridge is rebooting";
    dbgPrintln("[queue] " + why, LogLevel::LOG_ERROR);
    failAllQueued(why);
    publishStatus("error", why.c_str());
    delay(500);  // let the status publish go out
    ESP.restart();
  }

  while (!running && count > 0) {
    current = popFront();
    if (millis() - current.queuedMs > kMaxQueueWaitMs) {
      dbgPrintln("[queue] job " + current.id + " expired after waiting too long", LogLevel::LOG_WARN);
      publishJobStatus(current.id.c_str(), "failed", "expired while waiting in the queue");
      current.data.reset();
      continue;
    }
    publishStatus("printing", "");
    publishJobStatus(current.id.c_str(), "printing", "sending to printer");
    startedMs = millis();
    String err;
    if (!targetSendBegin(current.data.get(), current.len, err)) {
      running = true;  // so finishCurrent() reports it like any other failure
      finishCurrent(false, err);
      continue;
    }
    running = true;
  }
}

bool printQueueBusy() {
  return running || count > 0;
}

size_t printQueueWaiting() {
  return count;
}

int printQueueProgressPct() {
  if (!running) return -1;
  size_t sent = 0, total = 0;
  targetSendProgress(sent, total);
  if (total == 0) return 0;
  return (int)((sent * 100) / total);
}

const String& printQueueLastError() {
  return lastError;
}
