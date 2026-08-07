#include "target.h"

#ifdef TARGET_USB_HOST

#include <usb/usb_host.h>

#include <cstring>

#include "../logging.h"

// "usb_host" transport method: this board's ESP32-S2/S3 acts as a genuine
// USB host -- not the "usb" target's trick of reusing the board's own
// UART-over-programming-port (usb_target.cpp), which only works if the
// printer accepts a plain serial/UART connection. This enumerates whatever's
// plugged into the native USB port, claims it if it's a USB Printer-class
// device (bInterfaceClass 0x07), and forwards bytes to its bulk OUT
// endpoint as real USB bulk transfers. Ported from a standalone test sketch
// that validated this approach against a real Brother QL-720NW before this
// was integrated here (device enumeration/claim/transfer sequence, and the
// need for -D ARDUINO_USB_CDC_ON_BOOT=0 in platformio.ini -- without it the
// board's default native-USB-CDC auto-init fights usb_host_install() for
// the same OTG peripheral and bootloops).
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
static volatile bool transferOk = false;

// USB-IF Printer class code. Hardcoded rather than relying on a
// USB_CLASS_PRINTER macro -- not confirmed provided by the usb_host headers.
static const uint8_t USB_CLASS_PRINTER_ = 0x07;

const char* targetMethodName() { return "usb_host"; }

static void printerTransferCb(usb_transfer_t* transfer) {
  if (transfer->device_handle != deviceHandle) return;
  const bool isIn = transfer->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK;
  if (isIn) return;  // this firmware doesn't read the printer's status back
  transferOk = (transfer->status == 0);
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
      inPrinterInterface = (intf->bInterfaceClass == USB_CLASS_PRINTER_) &&
                           (intf->bInterfaceSubClass == 1) &&
                           (intf->bInterfaceProtocol == 1 || intf->bInterfaceProtocol == 2);
      if (inPrinterInterface && !printerInterfaceFound) {
        printerInterfaceFound = true;
        const esp_err_t err = usb_host_interface_claim(clientHandle, deviceHandle,
            intf->bInterfaceNumber, intf->bAlternateSetting);
        if (err == ESP_OK) {
          printerClaimed = true;
          dbgPrintln("[target] usb_host: printer interface claimed", LogLevel::LOG_INFO);
        } else {
          dbgPrint("[target] usb_host: usb_host_interface_claim failed: ");
          dbgPrintln(err, LogLevel::LOG_ERROR);
        }
      }
    } else if (bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && inPrinterInterface && printerClaimed) {
      const usb_ep_desc_t* ep = reinterpret_cast<const usb_ep_desc_t*>(p);
      const bool isBulk = (ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_BULK;
      const bool isOut = (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) == 0;
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
    }
  }
}

static void clientEventCallback(const usb_host_client_event_msg_t* eventMsg, void* arg) {
  if (eventMsg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    dbgPrintln("[target] usb_host: device gone", LogLevel::LOG_WARN);
    printerClaimed = false;
    printerInterfaceFound = false;
    if (printerOut != nullptr) {
      usb_host_transfer_free(printerOut);
      printerOut = nullptr;
    }
    return;
  }
  if (eventMsg->event != USB_HOST_CLIENT_EVENT_NEW_DEV) return;

  dbgPrint("[target] usb_host: new device address=");
  dbgPrintln(eventMsg->new_dev.address, LogLevel::LOG_INFO);

  const esp_err_t openErr = usb_host_device_open(clientHandle, eventMsg->new_dev.address, &deviceHandle);
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
  }
}

void targetSetup() {
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

void targetLoop() {
  uint32_t eventFlags;
  usb_host_lib_handle_events(0, &eventFlags);
  usb_host_client_handle_events(clientHandle, 0);
}

// Blocks (pumping USB events) until the in-flight transfer completes or
// times out. Per the USB host library's own contract: never resubmit or
// modify a transfer before its prior completion callback has fired.
static bool waitForTransfer(unsigned long timeoutMs) {
  const unsigned long start = millis();
  while (!transferDone) {
    targetLoop();
    if (millis() - start > timeoutMs) return false;
  }
  return transferOk;
}

// The USB device/interface claim persists across jobs (torn down only if the
// device itself disconnects, see USB_HOST_CLIENT_EVENT_DEV_GONE above), so
// there's nothing to release once a send completes -- unlike the network
// target's TCP socket.
bool targetSend(const uint8_t* data, size_t len, String& err) {
  if (len == 0) {
    err = "empty payload";
    return false;
  }
  if (!printerClaimed || printerOut == nullptr) {
    err = "no USB printer enumerated yet";
    return false;
  }

  size_t sent = 0;
  while (sent < len) {
    const size_t chunk = min(len - sent, (size_t)printerOut->data_buffer_size);
    memcpy(printerOut->data_buffer, data + sent, chunk);
    printerOut->num_bytes = chunk;
    transferDone = false;
    transferOk = false;
    const esp_err_t submitErr = usb_host_transfer_submit(printerOut);
    if (submitErr != ESP_OK) {
      err = "usb_host_transfer_submit failed: 0x" + String(submitErr, HEX);
      return false;
    }
    if (!waitForTransfer(2000)) {
      err = "usb bulk transfer timed out";
      return false;
    }
    sent += chunk;
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

#endif // TARGET_USB_HOST
