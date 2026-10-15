// Host-side simulation of the non-blocking USB send + print queue against a
// scripted fake printer (stub/usb/usb_host.h). Build:
//   g++ -std=c++17 -Istub -I<src> sim.cpp -Wl,--unresolved-symbols=ignore-all
#include <cassert>
#include <vector>

#include "config.h"
#include "logging.h"
#include "mqtt_bridge.h"
#include <usb/usb_host.h>

FakeUsb fake;
unsigned long fakeNow = 1000;
EspC ESP;
AppConfig cfg;
bool debugOutputEnabled = false;
String pendingLogLine;
LogLevel pendingLogLevel = LogLevel::LOG_DEBUG;
LineCapture lineCapture(&pendingLogLine);
static bool verbose = false;
void flushPendingLogLine() {
  if (verbose || pendingLogLevel <= LogLevel::LOG_WARN) printf("    %8lu  %s\n", fakeNow, pendingLogLine.c_str());
  pendingLogLine = "";
  pendingLogLevel = LogLevel::LOG_DEBUG;
}

struct Pub { std::string id, status, msg; };
static std::vector<Pub> jobPubs;
static std::vector<std::string> statusPubs;
void publishJobStatus(const char* id, const char* st, const char* m) { jobPubs.push_back({id, st, m}); }
void publishStatus(const char* phase, const char*) { statusPubs.push_back(phase); }

#include "targets/usb_host_target.cpp"
#include "targets/target.cpp"
#include "print_queue.cpp"

static std::unique_ptr<uint8_t[]> buf(size_t n) { std::unique_ptr<uint8_t[]> b(new uint8_t[n]); for (size_t i = 0; i < n; i++) b[i] = (uint8_t)i; return b; }
static std::string finalStatus(const std::string& id) { std::string s; for (auto& p : jobPubs) if (p.id == id) s = p.status; return s; }
static std::string finalMsg(const std::string& id) { std::string s; for (auto& p : jobPubs) if (p.id == id) s = p.msg; return s; }
// Runs loop() until the queue is idle (or a time budget is exhausted). Each
// pass advances the fake clock 5 ms; `hook` may change the fake mid-run.
static unsigned long runUntilIdle(unsigned long budgetMs, void (*hook)(unsigned long elapsed) = nullptr) {
  const unsigned long start = fakeNow;
  while (printQueueBusy() && fakeNow - start < budgetMs) {
    if (hook) hook(fakeNow - start);
    printQueueLoop();
    fakeNow += 5;
  }
  return fakeNow - start;
}
static void enqueue(const char* id, size_t n) { String err; bool ok = printQueueEnqueue(id, "ql", buf(n), n, err); assert(ok); }
#define CHECK(c) do { if (!(c)) { printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } else printf("  ok: %s\n", #c); } while (0)
static int fails = 0;
static void reset() { fake.mode = FakeUsb::ACCEPT; fake.flushReports = true; fake.bytesAccepted = 0; fake.notFinished = 0; }

int main() {
  cfg.printerMethod = "usb_host";
  usbHostTargetSetup();

  printf("1) two normal jobs back to back\n");
  reset(); enqueue("a", 2000); enqueue("b", 3000);
  runUntilIdle(5000);
  CHECK(finalStatus("a") == "done"); CHECK(finalStatus("b") == "done"); CHECK(fake.bytesAccepted == 5000);

  printf("2) printer accepts half of every transfer -> resumes without resending bytes\n");
  reset(); fake.mode = FakeUsb::PARTIAL; enqueue("c", 2000);
  runUntilIdle(5000);
  CHECK(finalStatus("c") == "done"); CHECK(fake.bytesAccepted == 2000);

  printf("3) printer stalls 5 s mid-job then recovers -> job completes, no 0x10c\n");
  reset(); enqueue("d", 4000);
  runUntilIdle(20000, [](unsigned long t) { fake.mode = (t > 20 && t < 5000) ? FakeUsb::STALL : FakeUsb::ACCEPT; });
  CHECK(finalStatus("d") == "done"); CHECK(fake.bytesAccepted == 4000); CHECK(fake.notFinished == 0);

  printf("4) printer stops for good -> job fails in ~10 s, the next job still prints\n");
  reset(); fake.mode = FakeUsb::STALL; enqueue("e", 4000);
  unsigned long t = runUntilIdle(120000);
  CHECK(finalStatus("e") == "failed"); CHECK(t < 16000); printf("     failed after %lu ms: %s\n", t, finalMsg("e").c_str());
  fake.mode = FakeUsb::ACCEPT; fake.bytesAccepted = 0; enqueue("f", 2000);
  runUntilIdle(5000);
  CHECK(finalStatus("f") == "done"); CHECK(fake.bytesAccepted == 2000); CHECK(fake.notFinished == 0);

  printf("5) USB errors (STALL status) for a while then OK -> recovers\n");
  reset(); enqueue("g", 2000);
  runUntilIdle(10000, [](unsigned long t) { fake.mode = t < 30 ? FakeUsb::ERROR : FakeUsb::ACCEPT; });
  CHECK(finalStatus("g") == "done"); CHECK(fake.bytesAccepted == 2000);

  printf("6) persistent USB error -> fails fast, reports a reason\n");
  reset(); fake.mode = FakeUsb::ERROR; enqueue("h", 2000);
  t = runUntilIdle(30000);
  CHECK(finalStatus("h") == "failed"); CHECK(t < 3000); printf("     failed after %lu ms: %s\n", t, finalMsg("h").c_str());

  printf("7) unplugged mid-job -> fails, next job re-claims the printer\n");
  reset(); enqueue("i", 6000);
  runUntilIdle(10000, [](unsigned long t) { if (t == 50) fake.unplugNow = true; });
  CHECK(finalStatus("i") == "failed"); printf("     %s\n", finalMsg("i").c_str());
  fake.bytesAccepted = 0; enqueue("j", 1000);
  runUntilIdle(5000);
  CHECK(finalStatus("j") == "done"); CHECK(fake.bytesAccepted == 1000);

  printf("8) queue full while the printer is stuck -> 5th job refused, others fail in turn\n");
  reset(); fake.mode = FakeUsb::STALL;
  enqueue("k1", 1000); printQueueLoop(); enqueue("k2", 1000); enqueue("k3", 1000); enqueue("k4", 1000); enqueue("k5", 1000);
  { String err; bool ok = printQueueEnqueue("k6", "ql", buf(10), 10, err); CHECK(!ok); printf("     refused: %s\n", err.c_str()); }
  t = runUntilIdle(200000);
  CHECK(finalStatus("k1") == "failed"); CHECK(finalStatus("k5") == "failed");
  CHECK(finalStatus("k2") == "failed"); printf("     all 5 resolved in %lu ms\n", t);

  printf("9) flushed transfer never reports back (wedged stack) -> transfer replaced, next job prints\n");
  reset(); fake.mode = FakeUsb::STALL; fake.flushReports = false; enqueue("w1", 2000);
  runUntilIdle(30000);
  CHECK(finalStatus("w1") == "failed");
  fake.mode = FakeUsb::ACCEPT; fake.bytesAccepted = 0; enqueue("w2", 2000);
  runUntilIdle(10000);
  CHECK(finalStatus("w2") == "done"); CHECK(fake.notFinished == 0); CHECK(!ESP.restarted);

  printf("10) stack stays wedged -> after the replacement budget, reboot is requested\n");
  reset(); fake.mode = FakeUsb::STALL; fake.flushReports = false;
  for (int i = 0; i < 6 && !ESP.restarted; i++) { std::string id = "z" + std::to_string(i); enqueue(id.c_str(), 2000); runUntilIdle(60000); printQueueLoop(); }
  CHECK(ESP.restarted);

  printf("\n%s (%d failed checks)\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", fails);
  return fails ? 1 : 0;
}
