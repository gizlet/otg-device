#include "esp_log.h"
#include "h2_esp_board.h"
#include "h2_esp_target_task_policy.h"
#include "otg_ble.h"
#include "otg_esp_vhci.h"
#include "otg_usb_bth.h"

static const char* TAG = "ble";

static h2_runtime_t* s_runtime;
static otg_ble_t s_app;

static void bridge(void* user) {
  (void)user;
  otg_ble_run(&s_app);
}

static void fail(const char* stage, int rc) {
  ESP_LOGE(TAG, "startup failed stage=%s rc=%d", stage, rc);
}

void app_main(void) {
  int rc = h2_esp_target_task_policy_install();
  if (rc != H2_PAL_OK) return fail("task-policy", rc);

  h2_runtime_config_t config;
  rc = h2_esp_board_runtime_config(&config);
  if (rc != H2_PAL_OK) return fail("board", rc);
  rc = h2_runtime_init(&config, &s_runtime);
  if (rc != H2_PAL_OK) return fail("runtime", rc);

  rc = otg_ble_init(&s_app, s_runtime);
  if (rc != H2_PAL_OK) return fail("app", rc);
  const otg_hci_port_t* controller = NULL;
  rc = otg_esp_vhci_start(otg_ble_controller_sink(&s_app), &controller);
  if (rc != H2_PAL_OK) return fail("controller", rc);
  const otg_hci_port_t* host = NULL;
  rc = otg_usb_bth_start(otg_ble_host_sink(&s_app), &host);
  if (rc != H2_PAL_OK) return fail("usb", rc);
  otg_ble_attach(&s_app, controller, host);

  const h2_pal_task_options_t options = {
      .name = OTG_BLE_TASK_NAME,
      .min_stack_size = OTG_BLE_TASK_STACK_SIZE,
  };
  h2_pal_task_t* task = NULL;
  rc = h2_pal_task_start(s_runtime->task, &options, bridge, NULL, &task);
  if (rc != H2_PAL_OK) return fail("bridge", rc);

  ESP_LOGI(TAG, "ble %s: USB Bluetooth controller on board %s",
           OTG_BLE_VERSION, s_runtime->board);
}
