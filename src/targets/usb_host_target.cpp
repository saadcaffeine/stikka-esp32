#include "target.h"

#include <usb/usb_host.h>

#include <cstring>

#include "../logging.h"

// "usb_host" transport method: this board's ESP32-S2/S3 acts as a genuine
// USB host, enumerating whatever's plugged into the native USB port, claims
// it if it's a USB Printer-class device (bInterfaceClass 0x07), and forwards
// bytes to its bulk OUT endpoint as real USB bulk transfers. Ported from a
// standalone test sketch that validated this approach against a real
// Brother QL-720NW before this was integrated here (device enumeration/
// claim/transfer sequence, and the need for -D ARDUINO_USB_CDC_ON_BOOT=0 in
// platformio.ini -- without it the board's default native-USB-CDC auto-init
// fights usb_host_install() for the same OTG peripheral and bootloops).
// Always compiled in -- usb_host_install()/usb_host_client_register() below
// only ever run when target.cpp's dispatcher calls usbHostTargetSetup(),
// i.e. only when cfg.printerMethod=="usb_host".
//
// Requires a board whose USB-C/USB port circuitry can actually source VBUS
// to a downstream device in host mode -- confirmed for esp32-s2-saola-1 via
// its schematic (Espressif ESP32-S2-SAOLA-1_V1.1_schematics.pdf); some
// compact S2/S3 boards are wired power-in-only and can't (e.g. UM TinyS2,
// ruled out this way before this board was chosen).

static usb_host_client_handle_t clientHandle;
static usb_device_handle_t deviceHandle;

static bool printerInterfaceFound = false;
static bool printerClaimed = false;

static usb_transfer_t* printerOut = nullptr;
static volatile bool transferDone = false;
static volatile usb_transfer_status_t transferStatus = USB_TRANSFER_STATUS_COMPLETED;
static volatile size_t transferActual = 0;
// printerOut has been submitted and its completion hasn't been handled yet.
// While true it must not be resubmitted or freed (USB host library contract).
static bool inFlight = false;
// Tag for a transfer handed off by replaceStuckTransfer(): if it ever
// completes, printerTransferCb() frees it instead of reporting it.
static int abandonedTag = 0;
static void* const kAbandonedTransfer = &abandonedTag;

// Interface number currently claimed via usb_host_interface_claim() (primary
// printer-class path or the fallback path below) -- needed by DEV_GONE
// handling to release it before closing the device. Without that release,
// the Host Library considers the device still in use by this client and
// won't fully free it: usb_host_device_addr_list_fill() keeps reporting the
// same stale address, and any usb_host_device_open() attempt on it fails
// with ESP_ERR_INVALID_STATE (0x103/259) -- permanently, not just for a
// short race window -- until a physical replug or reboot.
static uint8_t claimedInterfaceNumber = 0;

// Fallback path for a bulk-OUT-capable USB device that doesn't identify as a
// USB Printer-class interface (bInterfaceClass 0x07) -- confirmed hardware
// validation of the printer-class scan below only covers a Brother QL-720NW.
// Small integrated label printers (Seiko SLP-series among them) commonly
// ship a vendor-specific class and a proprietary desktop driver instead of
// conforming to the Printer Class spec, so the primary scan may find nothing
// to claim on such a device. This tracks the first bulk-OUT endpoint seen on
// *any* interface while walking the descriptor, so clientEventCallback() can
// fall back to claiming it if the printer-class scan comes up empty.
// Unvalidated against real Seiko hardware -- needs confirmation once a
// device is available; see stikka-esp32/README.md.
static bool fallbackEndpointFound = false;
static uint8_t fallbackInterfaceNumber = 0;
static uint8_t fallbackAltSetting = 0;
static uint8_t fallbackEndpointAddress = 0;
static uint16_t fallbackMaxPacketSize = 0;
static uint8_t currentInterfaceNumber = 0;
static uint8_t currentAltSetting = 0;

// USB-IF Printer class code. Hardcoded rather than relying on a
// USB_CLASS_PRINTER macro -- not confirmed provided by the usb_host headers.
static const uint8_t USB_CLASS_PRINTER_ = 0x07;

static void printerTransferCb(usb_transfer_t* transfer) {
  if (transfer->context == kAbandonedTransfer) {
    usb_host_transfer_free(transfer);
    return;
  }
  if (transfer != printerOut) return;
  const bool isIn = transfer->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK;
  if (isIn) return;  // this firmware doesn't read the printer's status back
  transferStatus = transfer->status;
  transferActual = transfer->actual_num_bytes;
  transferDone = true;
}

// Walks one USB config descriptor's TLV list looking for a Printer-class
// interface (subclass 1 = USB Printer Class Spec 1.1; protocol 1 unidirectional
// or 2 bidirectional -- 1284.4 bidirectional, protocol 3, isn't handled) and,
// once found, its bulk OUT endpoint.
static void scanConfigDescriptor(const usb_config_desc_t* configDesc) {
  const uint8_t* p = &configDesc->val[0];
  uint8_t bLength;
  bool inPrinterInterface = false;

  for (int i = 0; i < configDesc->wTotalLength; i += bLength, p += bLength) {
    bLength = *p;
    if (i + bLength > configDesc->wTotalLength) break;
    const uint8_t bDescriptorType = *(p + 1);

    if (bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const usb_intf_desc_t* intf = reinterpret_cast<const usb_intf_desc_t*>(p);
      currentInterfaceNumber = intf->bInterfaceNumber;
      currentAltSetting = intf->bAlternateSetting;
      dbgPrint("[target] usb_host: interface ");
      dbgPrint(intf->bInterfaceNumber);
      dbgPrint(" class=0x");
      dbgPrint(String(intf->bInterfaceClass, HEX));
      dbgPrint(" subclass=");
      dbgPrint(intf->bInterfaceSubClass);
      dbgPrint(" protocol=");
      dbgPrintln(intf->bInterfaceProtocol, LogLevel::LOG_DEBUG);
      inPrinterInterface = (intf->bInterfaceClass == USB_CLASS_PRINTER_) &&
                           (intf->bInterfaceSubClass == 1) &&
                           (intf->bInterfaceProtocol == 1 || intf->bInterfaceProtocol == 2);
      if (inPrinterInterface && !printerInterfaceFound) {
        printerInterfaceFound = true;
        const esp_err_t err = usb_host_interface_claim(clientHandle, deviceHandle,
            intf->bInterfaceNumber, intf->bAlternateSetting);
        if (err == ESP_OK) {
          printerClaimed = true;
          claimedInterfaceNumber = intf->bInterfaceNumber;
          dbgPrintln("[target] usb_host: printer interface claimed", LogLevel::LOG_INFO);
        } else {
          dbgPrint("[target] usb_host: usb_host_interface_claim failed: ");
          dbgPrintln(err, LogLevel::LOG_ERROR);
        }
      }
    } else if (bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
      const usb_ep_desc_t* ep = reinterpret_cast<const usb_ep_desc_t*>(p);
      const bool isBulk = (ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_BULK;
      const bool isOut = (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) == 0;
      if (inPrinterInterface && printerClaimed) {
        if (isBulk && isOut && printerOut == nullptr) {
          const int packetsPerTransfer = 8;
          const esp_err_t err = usb_host_transfer_alloc(ep->wMaxPacketSize * packetsPerTransfer, 0, &printerOut);
          if (err == ESP_OK) {
            printerOut->device_handle = deviceHandle;
            printerOut->bEndpointAddress = ep->bEndpointAddress;
            printerOut->callback = printerTransferCb;
            printerOut->context = nullptr;
            dbgPrint("[target] usb_host: bulk OUT endpoint ready, buffer=");
            dbgPrintln(printerOut->data_buffer_size, LogLevel::LOG_INFO);
          } else {
            dbgPrint("[target] usb_host: usb_host_transfer_alloc failed: ");
            dbgPrintln(err, LogLevel::LOG_ERROR);
          }
        }
      } else if (isBulk && isOut && !fallbackEndpointFound) {
        fallbackEndpointFound = true;
        fallbackInterfaceNumber = currentInterfaceNumber;
        fallbackAltSetting = currentAltSetting;
        fallbackEndpointAddress = ep->bEndpointAddress;
        fallbackMaxPacketSize = ep->wMaxPacketSize;
        dbgPrint("[target] usb_host: fallback bulk-OUT candidate on interface ");
        dbgPrintln(currentInterfaceNumber, LogLevel::LOG_DEBUG);
      }
    }
  }
}

// Opens the device at `address` (if not already open under this client) and
// attempts to claim a USB Printer-class interface on it, same descriptor
// scan/claim/fallback sequence as always ran inline in clientEventCallback's
// USB_HOST_CLIENT_EVENT_NEW_DEV case. Extracted so attemptUsbReconnect() below
// can run the exact same logic against a device this client never got a
// NEW_DEV notification for.
static void openAndClaimDevice(uint8_t address) {
  const esp_err_t openErr = usb_host_device_open(clientHandle, address, &deviceHandle);
  if (openErr != ESP_OK) {
    dbgPrint("[target] usb_host: usb_host_device_open failed: ");
    dbgPrintln(openErr, LogLevel::LOG_ERROR);
    return;
  }

  const usb_config_desc_t* configDesc;
  const esp_err_t descErr = usb_host_get_active_config_descriptor(deviceHandle, &configDesc);
  if (descErr != ESP_OK) {
    dbgPrint("[target] usb_host: usb_host_get_active_config_descriptor failed: ");
    dbgPrintln(descErr, LogLevel::LOG_ERROR);
    return;
  }

  scanConfigDescriptor(configDesc);
  if (!printerInterfaceFound) {
    dbgPrintln("[target] usb_host: no printer-class interface found on this device", LogLevel::LOG_WARN);
    if (fallbackEndpointFound) {
      dbgPrint("[target] usb_host: falling back to first bulk-OUT endpoint on interface ");
      dbgPrint(fallbackInterfaceNumber);
      dbgPrintln(" (unvalidated device path -- not USB Printer-class)", LogLevel::LOG_WARN);
      const esp_err_t claimErr = usb_host_interface_claim(clientHandle, deviceHandle,
          fallbackInterfaceNumber, fallbackAltSetting);
      if (claimErr == ESP_OK) {
        printerClaimed = true;
        printerInterfaceFound = true;
        claimedInterfaceNumber = fallbackInterfaceNumber;
        const int packetsPerTransfer = 8;
        const esp_err_t allocErr = usb_host_transfer_alloc(fallbackMaxPacketSize * packetsPerTransfer, 0, &printerOut);
        if (allocErr == ESP_OK) {
          printerOut->device_handle = deviceHandle;
          printerOut->bEndpointAddress = fallbackEndpointAddress;
          printerOut->callback = printerTransferCb;
          printerOut->context = nullptr;
          dbgPrint("[target] usb_host: fallback bulk OUT endpoint ready, buffer=");
          dbgPrintln(printerOut->data_buffer_size, LogLevel::LOG_INFO);
        } else {
          dbgPrint("[target] usb_host: fallback usb_host_transfer_alloc failed: ");
          dbgPrintln(allocErr, LogLevel::LOG_ERROR);
        }
      } else {
        dbgPrint("[target] usb_host: fallback usb_host_interface_claim failed: ");
        dbgPrintln(claimErr, LogLevel::LOG_ERROR);
      }
    }
  }
}

static void clientEventCallback(const usb_host_client_event_msg_t* eventMsg, void* arg) {
  if (eventMsg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    dbgPrintln("[target] usb_host: device gone", LogLevel::LOG_WARN);
    if (printerOut != nullptr) {
      if (inFlight && !transferDone) {
        printerOut->context = kAbandonedTransfer;  // still in flight: its callback frees it
      } else {
        usb_host_transfer_free(printerOut);
      }
      printerOut = nullptr;
      inFlight = false;
    }
    if (deviceHandle != nullptr) {
      // The client contract (usb_host_device_close()'s doc comment) requires
      // releasing any claimed interface before closing the device -- skip
      // this and the Host Library keeps the device object alive waiting for
      // a release that never comes, so it never gets freed even though it's
      // physically gone. That's what was causing the *permanent* (not just a
      // brief race) "usb_host_device_open failed: 259" (ESP_ERR_INVALID_STATE)
      // on every later reconnect attempt at the same address.
      if (printerClaimed) {
        const esp_err_t relErr = usb_host_interface_release(clientHandle, deviceHandle, claimedInterfaceNumber);
        dbgPrint("[target] usb_host: interface release on device-gone: ");
        dbgPrintln(relErr, LogLevel::LOG_WARN);
      }
      const esp_err_t closeErr = usb_host_device_close(clientHandle, deviceHandle);
      dbgPrint("[target] usb_host: device close on device-gone: ");
      dbgPrintln(closeErr, LogLevel::LOG_WARN);
      deviceHandle = nullptr;
    }
    printerClaimed = false;
    printerInterfaceFound = false;
    fallbackEndpointFound = false;
    return;
  }
  if (eventMsg->event != USB_HOST_CLIENT_EVENT_NEW_DEV) return;

  dbgPrint("[target] usb_host: new device address=");
  dbgPrintln(eventMsg->new_dev.address, LogLevel::LOG_INFO);
  openAndClaimDevice(eventMsg->new_dev.address);
}

void usbHostTargetSetup() {
  const usb_host_config_t hostConfig = {
    .intr_flags = ESP_INTR_FLAG_LEVEL1,
  };
  const esp_err_t installErr = usb_host_install(&hostConfig);
  dbgPrint("[target] usb_host: usb_host_install: ");
  dbgPrintln(installErr, LogLevel::LOG_INFO);

  const usb_host_client_config_t clientConfig = {
    .is_synchronous = false,
    .max_num_event_msg = 5,
    .async = {
      .client_event_callback = clientEventCallback,
      .callback_arg = nullptr,
    },
  };
  const esp_err_t clientErr = usb_host_client_register(&clientConfig, &clientHandle);
  dbgPrint("[target] usb_host: usb_host_client_register: ");
  dbgPrintln(clientErr, LogLevel::LOG_INFO);

  // Baseline reading, before WiFi/MQTT/TLS are up -- isolates how much the
  // USB host library itself costs vs. everything else that later competes
  // with it for heap.
  dbgPrint("[target] usb_host: post-init freeHeap=");
  dbgPrint((unsigned long)ESP.getFreeHeap());
  dbgPrint(" maxAlloc=");
  dbgPrintln((unsigned long)ESP.getMaxAllocHeap(), LogLevel::LOG_INFO);
}

void usbHostTargetLoop() {
  uint32_t eventFlags;
  usb_host_lib_handle_events(0, &eventFlags);
  usb_host_client_handle_events(clientHandle, 0);
}

// Called when a send is attempted with no printer currently claimed. Pumps
// pending USB host events first (targetLoop() already does this every
// loop() iteration, but a job can land in the same tick a device reappears)
// and, if that doesn't produce a claimed printer, proactively lists already-
// attached device addresses and tries each one. That second step matters
// because a NEW_DEV client event only fires for devices the Host Library
// enumerates *after* this client registered -- a printer that was already
// plugged in (or that reappeared while this client's event queue was full)
// would otherwise never get (re-)claimed until a physical unplug/replug.
static bool attemptUsbReconnect() {
  for (int i = 0; i < 5 && !printerClaimed; i++) {
    usbHostTargetLoop();
  }
  if (printerClaimed) return true;

  // A device that just went through USB_HOST_CLIENT_EVENT_DEV_GONE (or is
  // mid-bus-reset while re-enumerating) can sit in a transient state for a
  // short window where usb_host_device_addr_list_fill() still reports it but
  // usb_host_device_open() rejects it with ESP_ERR_INVALID_STATE -- observed
  // in practice as "usb_host_device_open failed: 259" immediately after a
  // "device gone" log, i.e. the Host Library hadn't finished tearing the old
  // device object down yet. Retry the whole probe a few times with a short
  // delay (pumping events in between, in case the retry gap is what it takes
  // for a queued NEW_DEV to land) rather than failing on the first race.
  const int maxAttempts = 5;
  const unsigned long retryDelayMs = 150;
  for (int attempt = 0; attempt < maxAttempts; attempt++) {
    if (attempt > 0) {
      delay(retryDelayMs);
      usbHostTargetLoop();
      if (printerClaimed) return true;
    }

    uint8_t addrList[8];
    int numDevices = 0;
    const esp_err_t fillErr = usb_host_device_addr_list_fill(8, addrList, &numDevices);
    if (fillErr != ESP_OK) {
      dbgPrint("[target] usb_host: reconnect: usb_host_device_addr_list_fill failed: ");
      dbgPrintln(fillErr, LogLevel::LOG_ERROR);
      continue;
    }
    if (numDevices == 0) {
      dbgPrintln("[target] usb_host: reconnect: no attached devices yet", LogLevel::LOG_WARN);
      continue;
    }

    dbgPrint("[target] usb_host: reconnect: probing ");
    dbgPrint(numDevices);
    dbgPrintln(" attached device(s)", LogLevel::LOG_INFO);

    for (int i = 0; i < numDevices && !printerClaimed; i++) {
      openAndClaimDevice(addrList[i]);
    }
    if (printerClaimed) return true;
  }
  dbgPrint("[target] usb_host: reconnect: giving up after ");
  dbgPrint(maxAttempts);
  dbgPrintln(" attempts", LogLevel::LOG_ERROR);
  return false;
}

// ---------------------------------------------------------------------------
// Non-blocking send. One job at a time, advanced from usbHostTargetSendPoll()
// on every loop() pass (via print_queue.cpp) so MQTT, the web UI and new jobs
// keep being serviced while a label goes out -- and so a printer that stops
// accepting data fails that one job cleanly instead of freezing the board.
//
// Per the USB host library's contract, a transfer may not be resubmitted (or
// freed) until its completion callback has run, and a transfer error halts
// the endpoint until it is flushed and cleared. The previous blocking version
// flushed a timed-out transfer but never pumped events afterwards, so the
// cancellation callback never ran and every later submit failed with
// ESP_ERR_NOT_FINISHED (0x10c) until a reboot. Here a timed-out transfer is
// halted + flushed, then we keep pumping until it reports back (counting any
// bytes the printer did accept, so nothing is sent twice) before clearing the
// endpoint and continuing. If it never reports back, the transfer object is
// abandoned and replaced; only if that also fails is the stack declared stuck
// (usbHostTargetNeedsReboot()).
// ---------------------------------------------------------------------------

static const unsigned long kChunkTimeoutMs = 3000;   // one bulk transfer with no completion
static const unsigned long kCancelWaitMs = 1000;     // time a flushed transfer gets to report back
static const unsigned long kIdleTimeoutMs = 10000;   // no bytes accepted at all for this long -> fail job
static const unsigned long kTotalTimeoutMs = 90000;  // hard cap per job
static const int kMaxChunkRetries = 3;               // consecutive failed attempts with zero progress
static const unsigned long kRetryBackoffMs = 250;    // x attempt number, after a USB error status
static const int kMaxAbandonedTransfers = 4;         // beyond this the stack is considered stuck

enum class UsbJobPhase : uint8_t { Idle, Sending, Cancelling };
static UsbJobPhase jobPhase = UsbJobPhase::Idle;
static const uint8_t* jobData = nullptr;
static size_t jobLen = 0;
static size_t jobSent = 0;
static size_t chunkLen = 0;
static unsigned long jobStartMs = 0;
static unsigned long lastProgressMs = 0;
static unsigned long chunkSubmitMs = 0;
static unsigned long cancelStartMs = 0;
static int chunkRetries = 0;
static unsigned long retryAtMs = 0;  // backoff after a USB error before the next submit
static int abandonedTransfers = 0;
static bool usbWedged = false;

// Flush whatever is queued on the bulk OUT endpoint and clear its halted
// state so it accepts transfers again. Only valid when no transfer of ours is
// in flight on it (callback already ran), otherwise use beginCancel().
static void clearHaltedEndpoint() {
  if (deviceHandle == nullptr || printerOut == nullptr) return;
  const uint8_t ep = printerOut->bEndpointAddress;
  usb_host_endpoint_halt(deviceHandle, ep);  // no-op if the error already halted it
  usb_host_endpoint_flush(deviceHandle, ep);
  const esp_err_t clearErr = usb_host_endpoint_clear(deviceHandle, ep);
  if (clearErr != ESP_OK) {
    dbgPrint("[target] usb_host: endpoint clear failed: 0x");
    dbgPrintln(String(clearErr, HEX), LogLevel::LOG_WARN);
  }
}

// Ask the library to cancel the in-flight transfer. Its callback then fires
// (status CANCELED, actual_num_bytes = what the printer did take) once events
// are pumped -- see the Cancelling phase in usbHostTargetSendPoll().
static void beginCancel() {
  if (deviceHandle == nullptr || printerOut == nullptr) return;
  const uint8_t ep = printerOut->bEndpointAddress;
  usb_host_endpoint_halt(deviceHandle, ep);
  usb_host_endpoint_flush(deviceHandle, ep);
  cancelStartMs = millis();
}

// Last resort for a transfer that never reports back after a flush: hand it
// over to printerTransferCb() to free whenever (if ever) it completes, and
// carry on with a freshly allocated transfer on the same endpoint.
static bool replaceStuckTransfer() {
  if (printerOut == nullptr) return false;
  if (abandonedTransfers >= kMaxAbandonedTransfers) {
    usbWedged = true;
    return false;
  }
  const size_t size = printerOut->data_buffer_size;
  const uint8_t ep = printerOut->bEndpointAddress;
  usb_transfer_t* fresh = nullptr;
  if (usb_host_transfer_alloc(size, 0, &fresh) != ESP_OK) {
    dbgPrintln("[target] usb_host: could not allocate a replacement transfer", LogLevel::LOG_ERROR);
    usbWedged = true;
    return false;
  }
  printerOut->context = kAbandonedTransfer;
  abandonedTransfers++;
  fresh->device_handle = deviceHandle;
  fresh->bEndpointAddress = ep;
  fresh->callback = printerTransferCb;
  fresh->context = nullptr;
  printerOut = fresh;
  inFlight = false;
  transferDone = false;
  dbgPrint("[target] usb_host: replaced a stuck transfer (abandoned so far: ");
  dbgPrint(abandonedTransfers);
  dbgPrintln(")", LogLevel::LOG_WARN);
  return true;
}

// Before starting a job, make sure the transfer object isn't still busy from
// a previous failed job (bounded wait, then replace it).
static bool settleTransfer(String& err) {
  if (!inFlight) return true;
  const unsigned long start = millis();
  while (!transferDone && millis() - start < kCancelWaitMs) {
    usbHostTargetLoop();
    delay(1);
  }
  if (transferDone) {
    inFlight = false;
    clearHaltedEndpoint();
    return true;
  }
  if (replaceStuckTransfer()) {
    clearHaltedEndpoint();
    return true;
  }
  err = "printer USB connection stuck (bridge needs a reboot)";
  return false;
}

static TargetSendState finishJob(bool ok, const String& why, String& err) {
  if (!ok) {
    err = why;
    // Leave nothing running on the endpoint for the next job. The callback
    // for this cancel is collected by settleTransfer() at the next begin.
    if (inFlight && !transferDone) beginCancel();
  }
  dbgPrint("[target] usb_host: job ");
  dbgPrint(ok ? "sent " : "failed at ");
  dbgPrint((unsigned long)jobSent);
  dbgPrint("/");
  dbgPrint((unsigned long)jobLen);
  dbgPrint(" bytes in ");
  dbgPrint(millis() - jobStartMs);
  dbgPrintln(" ms", ok ? LogLevel::LOG_INFO : LogLevel::LOG_ERROR);
  jobPhase = UsbJobPhase::Idle;
  jobData = nullptr;
  return ok ? TargetSendState::Done : TargetSendState::Failed;
}

bool usbHostTargetSendBegin(const uint8_t* data, size_t len, String& err) {
  if (jobPhase != UsbJobPhase::Idle) {
    err = "usb target busy with another job";
    return false;
  }
  if (len == 0) {
    err = "empty payload";
    return false;
  }
  if (!printerClaimed || printerOut == nullptr) {
    if (!attemptUsbReconnect()) {
      err = "no USB printer connected";
      return false;
    }
  }
  if (!settleTransfer(err)) return false;

  jobData = data;
  jobLen = len;
  jobSent = 0;
  chunkLen = 0;
  chunkRetries = 0;
  jobStartMs = lastProgressMs = retryAtMs = millis();
  jobPhase = UsbJobPhase::Sending;
  return true;
}

TargetSendState usbHostTargetSendPoll(String& err) {
  if (jobPhase == UsbJobPhase::Idle) return TargetSendState::Done;
  usbHostTargetLoop();
  const unsigned long now = millis();

  if (!printerClaimed || printerOut == nullptr) {
    return finishJob(false, "USB printer disconnected during the print", err);
  }

  if (jobPhase == UsbJobPhase::Cancelling) {
    if (transferDone) {
      inFlight = false;
      const size_t got = min((size_t)transferActual, chunkLen);
      jobSent += got;
      if (got > 0) {
        lastProgressMs = now;
        chunkRetries = 0;
      }
      clearHaltedEndpoint();
      jobPhase = UsbJobPhase::Sending;
    } else if (now - cancelStartMs > kCancelWaitMs) {
      dbgPrintln("[target] usb_host: cancelled transfer never reported back", LogLevel::LOG_WARN);
      if (!replaceStuckTransfer()) {
        return finishJob(false, "printer USB connection stuck (bridge will reboot)", err);
      }
      clearHaltedEndpoint();
      jobPhase = UsbJobPhase::Sending;
    } else {
      return TargetSendState::Running;
    }
  }

  // Sending
  if (inFlight) {
    if (transferDone) {
      inFlight = false;
      const size_t got = min((size_t)transferActual, chunkLen);
      jobSent += got;
      if (got > 0) lastProgressMs = now;
      if (transferStatus == USB_TRANSFER_STATUS_COMPLETED) {
        chunkRetries = 0;
      } else if (transferStatus == USB_TRANSFER_STATUS_NO_DEVICE) {
        return finishJob(false, "USB printer disconnected during the print", err);
      } else {
        if (got == 0) chunkRetries++;
        dbgPrint("[target] usb_host: transfer ended with status ");
        dbgPrint((int)transferStatus);
        dbgPrintln(", clearing endpoint and resuming", LogLevel::LOG_WARN);
        clearHaltedEndpoint();
        if (chunkRetries > kMaxChunkRetries) {
          return finishJob(false, "printer rejected the data (USB status " + String((int)transferStatus) + ")", err);
        }
        retryAtMs = now + kRetryBackoffMs * chunkRetries;
      }
    } else if (now - chunkSubmitMs > kChunkTimeoutMs) {
      chunkRetries++;
      dbgPrintln("[target] usb_host: printer not accepting data, cancelling transfer", LogLevel::LOG_WARN);
      beginCancel();
      jobPhase = UsbJobPhase::Cancelling;
      return TargetSendState::Running;
    } else {
      return TargetSendState::Running;  // still in flight, nothing to do
    }
  }

  if (jobSent >= jobLen) return finishJob(true, "", err);
  if (now - lastProgressMs > kIdleTimeoutMs) {
    return finishJob(false, "printer stopped accepting data (check labels, cover and power)", err);
  }
  if (now - jobStartMs > kTotalTimeoutMs) {
    return finishJob(false, "print took longer than 90 s, gave up", err);
  }

  if ((long)(retryAtMs - now) > 0) return TargetSendState::Running;  // backing off after an error

  chunkLen = min(jobLen - jobSent, (size_t)printerOut->data_buffer_size);
  memcpy(printerOut->data_buffer, jobData + jobSent, chunkLen);
  printerOut->num_bytes = chunkLen;
  transferDone = false;
  transferActual = 0;
  const esp_err_t submitErr = usb_host_transfer_submit(printerOut);
  if (submitErr == ESP_OK) {
    inFlight = true;
    chunkSubmitMs = now;
    return TargetSendState::Running;
  }

  chunkRetries++;
  dbgPrint("[target] usb_host: transfer submit failed: 0x");
  dbgPrintln(String(submitErr, HEX), LogLevel::LOG_WARN);
  if (submitErr == ESP_ERR_NOT_FINISHED) {
    // The library still counts this transfer as in flight: never reuse it.
    if (!replaceStuckTransfer()) {
      return finishJob(false, "printer USB connection stuck (bridge will reboot)", err);
    }
  }
  clearHaltedEndpoint();
  if (chunkRetries > kMaxChunkRetries) {
    return finishJob(false, "USB submit failed: 0x" + String(submitErr, HEX), err);
  }
  retryAtMs = now + kRetryBackoffMs * chunkRetries;
  return TargetSendState::Running;
}

void usbHostTargetSendProgress(size_t& sent, size_t& total) {
  sent = jobSent;
  total = jobLen;
}

bool usbHostTargetNeedsReboot() {
  return usbWedged;
}

// Blocking convenience wrapper (bounded by kTotalTimeoutMs). Not used by the
// MQTT or web paths any more -- those go through print_queue.cpp -- but kept
// so targetSend() keeps working for anything else.
bool usbHostTargetSend(const uint8_t* data, size_t len, String& err) {
  if (!usbHostTargetSendBegin(data, len, err)) return false;
  for (;;) {
    const TargetSendState state = usbHostTargetSendPoll(err);
    if (state != TargetSendState::Running) return state == TargetSendState::Done;
    delay(1);
  }
}

bool usbHostTargetSendString(const String& body, String& err) {
  if (body.length() == 0) {
    err = "empty zpl payload";
    return false;
  }
  return usbHostTargetSend(reinterpret_cast<const uint8_t*>(body.c_str()), body.length(), err);
}

bool usbHostTargetReady() {
  return printerClaimed && printerOut != nullptr;
}
