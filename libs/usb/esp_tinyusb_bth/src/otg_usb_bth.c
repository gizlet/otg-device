#include "otg_usb_bth.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "sdkconfig.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#if !CONFIG_TINYUSB_BTH_ENABLED || CONFIG_TINYUSB_BTH_ISO_ALT_COUNT != 1
#error "otg_usb_bth needs CONFIG_TINYUSB_BTH_ENABLED and one SCO alternate"
#endif

/* Fixed ID for every development board running this app. Hosts bind the
 * Bluetooth class driver by class, not by ID. */
#define OTG_USB_BTH_VID 0x303Au
#define OTG_USB_BTH_PID 0x40E0u

#define EP_EVENT 0x81u
#define EP_ACL_OUT 0x02u
#define EP_ACL_IN 0x82u
#define EP_SCO_OUT 0x03u
#define EP_SCO_IN 0x83u
#define EVENT_EP_SIZE 16u
#define ACL_EP_SIZE 64u

enum { STR_LANG, STR_MANUFACTURER, STR_PRODUCT, STR_SERIAL, STR_COUNT };

static const char* TAG = "otg_bth";

/* ---- descriptors -------------------------------------------------------- */

static const tusb_desc_device_t s_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_WIRELESS_CONTROLLER,
    .bDeviceSubClass = TUD_BT_APP_SUBCLASS,
    .bDeviceProtocol = TUD_BT_PROTOCOL_PRIMARY_CONTROLLER,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = OTG_USB_BTH_VID,
    .idProduct = OTG_USB_BTH_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

#define CONFIG_TOTAL_LEN \
  (TUD_CONFIG_DESC_LEN + 9 + 7 + 7 + 7 + (9 + 7 + 7))

/* Bluetooth Core Vol 4 Part B: interface 0 has the event and ACL endpoints,
 * interface 1 the isochronous SCO endpoints (zero bandwidth in alternate 0).
 * TUD_BTH_ISO_ITF() starts with its own separating comma. */
static const uint8_t s_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CONFIG_TOTAL_LEN, 0, 100),
    TUD_BTH_PRI_ITF(0, 0, EP_EVENT, EVENT_EP_SIZE, 1, EP_ACL_IN, EP_ACL_OUT,
                    ACL_EP_SIZE)
    TUD_BTH_ISO_ITF(1, 0, EP_SCO_IN, EP_SCO_OUT, 0)};

_Static_assert(sizeof(s_config) == CONFIG_TOTAL_LEN, "config descriptor size");

static char s_serial[13];
static const char* s_strings[STR_COUNT] = {
    [STR_LANG] = (const char[]){0x09, 0x04},
    [STR_MANUFACTURER] = "Gizlet",
    [STR_PRODUCT] = "OTG Device BLE",
    [STR_SERIAL] = s_serial,
};

/* ---- state -------------------------------------------------------------- */

static const otg_hci_sink_t* s_sink;
static otg_hci_acl_reassembler_t s_acl_out;
static atomic_bool s_event_busy;
static atomic_bool s_acl_busy;
/* IN buffers stay owned by TinyUSB until the matching sent callback. */
static uint8_t s_event_buf[OTG_HCI_PACKET_MAX];
static uint8_t s_acl_buf[OTG_HCI_PACKET_MAX];

static void notify_writable(void) {
  if (s_sink != NULL && s_sink->writable != NULL) {
    s_sink->writable(s_sink->user);
  }
}

/* ---- TinyUSB class callbacks (TinyUSB task) ----------------------------- */

void tud_bt_hci_cmd_cb(void* hci_cmd, size_t cmd_len) {
  if (s_sink == NULL || s_sink->receive == NULL ||
      cmd_len < OTG_HCI_COMMAND_HEADER_LEN) {
    return;
  }
  s_sink->receive(s_sink->user, OTG_HCI_COMMAND, hci_cmd, cmd_len);
}

void tud_bt_acl_data_received_cb(void* acl_data, uint16_t data_len) {
  otg_hci_acl_feed(&s_acl_out, acl_data, data_len, s_sink);
}

void tud_bt_event_sent_cb(uint16_t sent_bytes) {
  (void)sent_bytes;
  atomic_store(&s_event_busy, false);
  notify_writable();
}

void tud_bt_acl_data_sent_cb(uint16_t sent_bytes) {
  (void)sent_bytes;
  atomic_store(&s_acl_busy, false);
  notify_writable();
}

static void on_usb_event(tinyusb_event_t* event, void* arg) {
  (void)arg;
  /* Only detach and (re)configuration abandon in-flight transfers and
   * partial input. Suspend and resume keep TinyUSB's transfers pending, so
   * IN buffers stay owned by TinyUSB and ACL reassembly continues. */
  switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
    case TINYUSB_EVENT_DETACHED:
      break;
    default:
      return;
  }
  otg_hci_acl_reset(&s_acl_out);
  atomic_store(&s_event_busy, false);
  atomic_store(&s_acl_busy, false);
  if (event->id == TINYUSB_EVENT_ATTACHED) {
    ESP_LOGI(TAG, "host configured the Bluetooth interface");
    notify_writable();
  }
}

/* ---- port to the host --------------------------------------------------- */

static h2_pal_result_t start_in(atomic_bool* busy, uint8_t* buffer,
                                const uint8_t* data, size_t len,
                                bool (*send)(void*, uint16_t)) {
  /* Only an unconfigured device drops the packet. A suspended bus keeps the
   * device mounted but refuses transfers; the packet stays queued and the
   * relay retries it after resume. */
  if (!tud_mounted()) return H2_PAL_ERR_CLOSED;
  if (!tud_ready()) return H2_PAL_ERR_WOULD_BLOCK;
  bool expected = false;
  if (!atomic_compare_exchange_strong(busy, &expected, true)) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  memcpy(buffer, data, len);
  if (!send(buffer, (uint16_t)len)) {
    /* Refused submissions are transient (suspend racing this call, or an
     * endpoint still claimed): keep the packet for the next pump. */
    atomic_store(busy, false);
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t port_send(void* user, otg_hci_type_t type,
                                 const uint8_t* data, size_t len) {
  (void)user;
  if (len == 0u || len > OTG_HCI_PACKET_MAX) return H2_PAL_ERR_INVALID_ARG;
  switch (type) {
    case OTG_HCI_EVENT:
      return start_in(&s_event_busy, s_event_buf, data, len,
                      tud_bt_event_send);
    case OTG_HCI_ACL:
      return start_in(&s_acl_busy, s_acl_buf, data, len,
                      tud_bt_acl_data_send);
    default:
      return H2_PAL_ERR_UNSUPPORTED;
  }
}

static const otg_hci_port_t s_port = {.send = port_send};

/* ---- start -------------------------------------------------------------- */

h2_pal_result_t otg_usb_bth_start(const otg_hci_sink_t* sink,
                                  const otg_hci_port_t** out_port) {
  if (sink == NULL || out_port == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (s_sink != NULL) return H2_PAL_ERR_INVALID_STATE;
  s_sink = sink;
  otg_hci_acl_reset(&s_acl_out);

  uint8_t mac[6];
  if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) memset(mac, 0, sizeof(mac));
  (void)snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
  config.descriptor.device = &s_device;
  config.descriptor.string = s_strings;
  config.descriptor.string_count = STR_COUNT;
  config.descriptor.full_speed_config = s_config;
  config.event_cb = on_usb_event;
  if (tinyusb_driver_install(&config) != ESP_OK) {
    s_sink = NULL;
    return H2_PAL_ERR_IO;
  }
  *out_port = &s_port;
  return H2_PAL_OK;
}
