#pragma once

#include <Arduino.h>

#include <memory>

// Small FIFO of decoded print jobs, drained one at a time from loop() via the
// non-blocking targetSendBegin()/targetSendPoll() API (targets/target.h).
// Lets onMqttMessage() and the web UI's test print hand a job off and return
// immediately, so a slow or stuck printer never freezes MQTT keepalives, the
// web UI, or later jobs. Each job ends as "done" or "failed" (published on the
// status topic as a job status), and the next queued job starts right after.

// Takes ownership of `data`. Returns false (with err set) when the queue is
// full; the caller should report the job as failed.
bool printQueueEnqueue(const String& jobId, const String& tag, std::unique_ptr<uint8_t[]> data, size_t len, String& err);

// Advances the current job and starts the next one. Call once per loop().
void printQueueLoop();

// True while a job is being sent or waiting in the queue.
bool printQueueBusy();

// For /status.json: number of jobs waiting (not counting the one being sent),
// progress of the current job (0-100, -1 when idle), and the error of the most
// recent failed job (cleared by the next successful one).
size_t printQueueWaiting();
int printQueueProgressPct();
const String& printQueueLastError();
