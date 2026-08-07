#include "target.h"

#ifdef TARGET_SERIAL

#include "../config.h"
#include "../logging.h"

// "serial" transport method: relays bytes to a dedicated hardware UART
// (UART2, on cfg.printerUartTxPin/printerUartRxPin) wired straight to the
// printer's serial input -- distinct from the debug UART (logging.cpp) and
// from the board's USB/programming port (usb_target.cpp), so print traffic
// never shares a wire with anything else.
static HardwareSerial printerUart(2);
static bool uartStarted = false;

const char* targetMethodName() { return "serial"; }

void targetSetup() {
  if (cfg.printerUartTxPin < 0 || cfg.printerUartRxPin < 0) {
    dbgPrintln("[target] serial target TX/RX pin not configured", LogLevel::LOG_ERROR);
    return;
  }
  printerUart.begin(cfg.printerUartBaud, SERIAL_8N1, cfg.printerUartRxPin, cfg.printerUartTxPin);
  uartStarted = true;
  dbgPrint("[target] serial target UART2 TX=");
  dbgPrint(cfg.printerUartTxPin);
  dbgPrint(" RX=");
  dbgPrint(cfg.printerUartRxPin);
  dbgPrint(" baud=");
  dbgPrintln(cfg.printerUartBaud, LogLevel::LOG_INFO);
}

void targetLoop() {
  // Nothing to pump -- HardwareSerial's own blocking calls handle everything.
}

bool targetSend(const uint8_t* data, size_t len, String& err) {
  if (len == 0) {
    err = "empty payload";
    return false;
  }
  if (!uartStarted) {
    err = "serial target UART not configured";
    return false;
  }
  const bool ok = writeStreamWithTimeout(printerUart, data, len, 256, err);
  printerUart.flush();
  return ok;
}

bool targetSendString(const String& body, String& err) {
  if (body.length() == 0) {
    err = "empty zpl payload";
    return false;
  }
  return targetSend(reinterpret_cast<const uint8_t*>(body.c_str()), body.length(), err);
}

#endif // TARGET_SERIAL
