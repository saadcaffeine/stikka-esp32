#pragma once
// Minimal fake of ESP-IDF's usb_host.h for host-side testing of
// usb_host_target.cpp's send state machine. Behaviour is scripted via `fake`.
#include <cstdint>
#include <cstddef>
#include <cstdlib>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FINISHED 0x10c
#define ESP_INTR_FLAG_LEVEL1 1
#define USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK 0x80
#define USB_B_DESCRIPTOR_TYPE_INTERFACE 4
#define USB_B_DESCRIPTOR_TYPE_ENDPOINT 5
#define USB_BM_ATTRIBUTES_XFERTYPE_MASK 3
#define USB_BM_ATTRIBUTES_XFER_BULK 2
typedef struct dev_s* usb_device_handle_t;
typedef struct cli_s* usb_host_client_handle_t;
typedef enum { USB_TRANSFER_STATUS_COMPLETED, USB_TRANSFER_STATUS_ERROR, USB_TRANSFER_STATUS_TIMED_OUT, USB_TRANSFER_STATUS_CANCELED, USB_TRANSFER_STATUS_STALL, USB_TRANSFER_STATUS_OVERFLOW, USB_TRANSFER_STATUS_SKIPPED, USB_TRANSFER_STATUS_NO_DEVICE } usb_transfer_status_t;
struct usb_transfer_t;
typedef void (*usb_transfer_cb_t)(usb_transfer_t*);
struct usb_transfer_t { uint8_t* data_buffer; size_t data_buffer_size; int num_bytes; int actual_num_bytes; uint32_t flags; usb_device_handle_t device_handle; uint8_t bEndpointAddress; usb_transfer_status_t status; uint32_t timeout_ms; usb_transfer_cb_t callback; void* context; };
struct usb_config_desc_t { uint16_t wTotalLength; uint8_t val[64]; };
struct usb_intf_desc_t { uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints, bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface; };
struct usb_ep_desc_t { uint8_t bLength, bDescriptorType, bEndpointAddress, bmAttributes; uint16_t wMaxPacketSize; uint8_t bInterval; };
typedef enum { USB_HOST_CLIENT_EVENT_NEW_DEV, USB_HOST_CLIENT_EVENT_DEV_GONE } usb_host_client_event_t;
struct usb_host_client_event_msg_t { usb_host_client_event_t event; struct { uint8_t address; } new_dev; struct { usb_device_handle_t dev_hdl; } dev_gone; };
typedef void (*usb_host_client_event_cb_t)(const usb_host_client_event_msg_t*, void*);
struct usb_host_config_t { int intr_flags; };
struct usb_host_client_config_t { bool is_synchronous; int max_num_event_msg; struct { usb_host_client_event_cb_t client_event_callback; void* callback_arg; } async; };

// ---- scripted fake device ----
struct FakeUsb {
  // accept: transfer completes on next event pump with all bytes
  // partial: completes with half the bytes
  // stall: never completes until flushed
  // error: completes with STALL status, 0 bytes
  enum Mode { ACCEPT, PARTIAL, STALL, ERROR } mode = ACCEPT;
  bool flushReports = true;      // false = a flushed transfer never calls back (wedge)
  usb_transfer_t* pending = nullptr;   // submitted, not yet called back
  bool completeNext = false;
  usb_transfer_status_t nextStatus = USB_TRANSFER_STATUS_COMPLETED;
  int nextActual = 0;
  size_t bytesAccepted = 0;
  int submits = 0, notFinished = 0, allocs = 0, frees = 0, clears = 0;
  usb_host_client_event_cb_t clientCb = nullptr;
  bool unplugNow = false;
};
extern FakeUsb fake;

static inline esp_err_t usb_host_install(const usb_host_config_t*) { return ESP_OK; }
static inline esp_err_t usb_host_client_register(const usb_host_client_config_t* c, usb_host_client_handle_t* h) { fake.clientCb = c->async.client_event_callback; *h = (usb_host_client_handle_t)1; return ESP_OK; }
static inline esp_err_t usb_host_lib_handle_events(int, uint32_t*) { return ESP_OK; }
static inline esp_err_t usb_host_client_handle_events(usb_host_client_handle_t, int) {
  if (fake.unplugNow) {
    fake.unplugNow = false;
    if (fake.pending) { usb_transfer_t* t = fake.pending; fake.pending = nullptr; t->status = USB_TRANSFER_STATUS_NO_DEVICE; t->actual_num_bytes = 0; t->callback(t); }
    usb_host_client_event_msg_t m{}; m.event = USB_HOST_CLIENT_EVENT_DEV_GONE; fake.clientCb(&m, nullptr);
    return ESP_OK;
  }
  if (fake.pending && fake.completeNext) {
    usb_transfer_t* t = fake.pending; fake.pending = nullptr; fake.completeNext = false;
    t->status = fake.nextStatus; t->actual_num_bytes = fake.nextActual;
    fake.bytesAccepted += fake.nextActual;
    t->callback(t);
  }
  return ESP_OK;
}
static inline esp_err_t usb_host_transfer_alloc(size_t n, int, usb_transfer_t** out) {
  usb_transfer_t* t = (usb_transfer_t*)calloc(1, sizeof(usb_transfer_t)); t->data_buffer = (uint8_t*)calloc(1, n); t->data_buffer_size = n; *out = t; fake.allocs++; return ESP_OK; }
static inline esp_err_t usb_host_transfer_free(usb_transfer_t* t) { if (t == fake.pending) abort(); free(t->data_buffer); free(t); fake.frees++; return ESP_OK; }
static inline esp_err_t usb_host_transfer_submit(usb_transfer_t* t) {
  if (fake.pending == t) { fake.notFinished++; return ESP_ERR_NOT_FINISHED; }
  fake.submits++;
  fake.pending = t;
  switch (fake.mode) {
    case FakeUsb::ACCEPT: fake.completeNext = true; fake.nextStatus = USB_TRANSFER_STATUS_COMPLETED; fake.nextActual = t->num_bytes; break;
    case FakeUsb::PARTIAL: fake.completeNext = true; fake.nextStatus = USB_TRANSFER_STATUS_COMPLETED; fake.nextActual = (t->num_bytes + 1) / 2; break;
    case FakeUsb::STALL: fake.completeNext = false; break;
    case FakeUsb::ERROR: fake.completeNext = true; fake.nextStatus = USB_TRANSFER_STATUS_STALL; fake.nextActual = 0; break;
  }
  return ESP_OK;
}
static inline esp_err_t usb_host_endpoint_halt(usb_device_handle_t, uint8_t) { return ESP_OK; }
static inline esp_err_t usb_host_endpoint_flush(usb_device_handle_t, uint8_t) {
  if (fake.pending && fake.flushReports) { fake.completeNext = true; fake.nextStatus = USB_TRANSFER_STATUS_CANCELED; fake.nextActual = 0; }
  return ESP_OK; }
static inline esp_err_t usb_host_endpoint_clear(usb_device_handle_t, uint8_t) { fake.clears++; return ESP_OK; }
static inline esp_err_t usb_host_interface_claim(usb_host_client_handle_t, usb_device_handle_t, uint8_t, uint8_t) { return ESP_OK; }
static inline esp_err_t usb_host_interface_release(usb_host_client_handle_t, usb_device_handle_t, uint8_t) { return ESP_OK; }
static inline esp_err_t usb_host_device_open(usb_host_client_handle_t, uint8_t, usb_device_handle_t* d) { *d = (usb_device_handle_t)2; return ESP_OK; }
static inline esp_err_t usb_host_device_close(usb_host_client_handle_t, usb_device_handle_t) { return ESP_OK; }
static inline esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t, const usb_config_desc_t** c) {
  static usb_config_desc_t cfgd; static bool init = false;
  if (!init) { init = true; usb_intf_desc_t i{9, 4, 0, 0, 1, 7, 1, 2, 0}; usb_ep_desc_t e{7, 5, 0x02, 2, 64, 0};
    memcpy(cfgd.val, &i, 9); memcpy(cfgd.val + 9, &e, 7); cfgd.wTotalLength = 16; }
  *c = &cfgd; return ESP_OK; }
static inline esp_err_t usb_host_device_addr_list_fill(int, uint8_t* l, int* n) { l[0] = 1; *n = 1; return ESP_OK; }
