#include "otg_esp_vhci.h"

#include "esp_bt.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include <string.h>

#if !CONFIG_BT_CONTROLLER_ONLY
#error "otg_esp_vhci needs CONFIG_BT_CONTROLLER_ONLY"
#endif

static const char* TAG = "otg_vhci";

static const otg_hci_sink_t* s_sink;
/* One H4 packet: indicator byte plus the HCI packet. Only the bridge task
 * sends, and the controller copies the packet before returning. */
static uint8_t s_tx[1u + OTG_HCI_PACKET_MAX];

/* ---- controller callbacks (controller task) ----------------------------- */

static void on_send_available(void) {
  if (s_sink != NULL && s_sink->writable != NULL) {
    s_sink->writable(s_sink->user);
  }
}

/* The sink may wait here until the host side has room; the controller keeps
 * later packets in its own buffers meanwhile. */
static int on_receive(uint8_t* data, uint16_t len) {
  if (s_sink == NULL || s_sink->receive == NULL || data == NULL || len < 2u) {
    return 0;
  }
  s_sink->receive(s_sink->user, (otg_hci_type_t)data[0], data + 1,
                  (size_t)len - 1u);
  return 0;
}

static const esp_vhci_host_callback_t s_callbacks = {
    .notify_host_send_available = on_send_available,
    .notify_host_recv = on_receive,
};

/* ---- port to the controller --------------------------------------------- */

static h2_pal_result_t port_send(void* user, otg_hci_type_t type,
                                 const uint8_t* data, size_t len) {
  (void)user;
  if (type != OTG_HCI_COMMAND && type != OTG_HCI_ACL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  if (len == 0u || len > OTG_HCI_PACKET_MAX) return H2_PAL_ERR_INVALID_ARG;
  if (!esp_vhci_host_check_send_available()) return H2_PAL_ERR_WOULD_BLOCK;
  s_tx[0] = (uint8_t)type;
  memcpy(s_tx + 1, data, len);
  esp_vhci_host_send_packet(s_tx, (uint16_t)(len + 1u));
  return H2_PAL_OK;
}

static const otg_hci_port_t s_port = {.send = port_send};

/* ---- start -------------------------------------------------------------- */

static h2_pal_result_t map_error(esp_err_t err) {
  switch (err) {
    case ESP_OK: return H2_PAL_OK;
    case ESP_ERR_NO_MEM: return H2_PAL_ERR_NO_MEMORY;
    case ESP_ERR_INVALID_STATE: return H2_PAL_ERR_INVALID_STATE;
    default: return H2_PAL_ERR_IO;
  }
}

h2_pal_result_t otg_esp_vhci_start(const otg_hci_sink_t* sink,
                                   const otg_hci_port_t** out_port) {
  if (sink == NULL || out_port == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (s_sink != NULL) return H2_PAL_ERR_INVALID_STATE;

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "erasing NVS: %s", esp_err_to_name(err));
    err = nvs_flash_erase();
    if (err == ESP_OK) err = nvs_flash_init();
  }
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return map_error(err);

  esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  err = esp_bt_controller_init(&config);
  if (err == ESP_OK) err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "controller start failed: %s", esp_err_to_name(err));
    return map_error(err);
  }
  s_sink = sink;
  err = esp_vhci_host_register_callback(&s_callbacks);
  if (err != ESP_OK) {
    s_sink = NULL;
    return map_error(err);
  }
  *out_port = &s_port;
  ESP_LOGI(TAG, "BLE controller ready on VHCI");
  return H2_PAL_OK;
}
