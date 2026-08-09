#include "target.h"

#include "../config.h"
#include "../logging.h"

// "serial" transport method: relays bytes to a dedicated hardware UART
// (UART2, on cfg.printerUartTxPin/printerUartRxPin) wired straight to the
// printer's serial input -- distinct from the debug output port (always
// UART0/the programming port, see logging.cpp), so print traffic never
// shares a wire with anything else. Always compiled in -- only called by
// target.cpp's dispatcher when cfg.printerMethod=="serial". Constructing
// this HardwareSerial is safe even when serial isn't the selected method:
// it doesn't touch hardware until .begin() below, in serialTargetSetup().
static HardwareSerial printerUart(2);
static bool uartStarted = false;

void serialTargetSetup() {
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

void serialTargetLoop() {
  // Nothing to pump -- HardwareSerial's own blocking calls handle everything.
}

bool serialTargetSend(const uint8_t* data, size_t len, String& err) {
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

bool serialTargetSendString(const String& body, String& err) {
  if (body.length() == 0) {
    err = "empty zpl payload";
    return false;
  }
  return serialTargetSend(reinterpret_cast<const uint8_t*>(body.c_str()), body.length(), err);
}
