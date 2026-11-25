#include "otg_ble.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                 \
    }                                                             \
  } while (0)

/* ---- fake sync --------------------------------------------------------- */

static int g_lock_depth;
static int g_wakes;
static h2_pal_mutex_t* const kMutex = (h2_pal_mutex_t*)&g_lock_depth;
static h2_pal_semaphore_t* const kSem = (h2_pal_semaphore_t*)&g_wakes;

static h2_pal_result_t create_mutex(void* user,
                                    const h2_pal_mutex_config_t* config,
                                    h2_pal_mutex_t** out) {
  (void)user;
  (void)config;
  *out = kMutex;
  return H2_PAL_OK;
}
static h2_pal_result_t destroy_mutex(void* user, h2_pal_mutex_t* mutex) {
  (void)user;
  (void)mutex;
  return H2_PAL_OK;
}
static h2_pal_result_t lock_mutex(void* user, h2_pal_mutex_t* mutex) {
  (void)user;
  (void)mutex;
  g_lock_depth++;
  CHECK(g_lock_depth == 1);
  return H2_PAL_OK;
}
static h2_pal_result_t unlock_mutex(void* user, h2_pal_mutex_t* mutex) {
  (void)user;
  (void)mutex;
  g_lock_depth--;
  return H2_PAL_OK;
}
static h2_pal_result_t create_sem(void* user,
                                  const h2_pal_semaphore_config_t* config,
                                  h2_pal_semaphore_t** out) {
  (void)user;
  (void)config;
  *out = kSem;
  return H2_PAL_OK;
}
static h2_pal_result_t destroy_sem(void* user, h2_pal_semaphore_t* sem) {
  (void)user;
  (void)sem;
  return H2_PAL_OK;
}
static h2_pal_result_t give_sem(void* user, h2_pal_semaphore_t* sem) {
  (void)user;
  (void)sem;
  g_wakes++;
  return H2_PAL_OK;
}
static const h2_pal_sync_vtable_t kSyncVtable = {
    .create_mutex = create_mutex,
    .destroy_mutex = destroy_mutex,
    .lock_mutex = lock_mutex,
    .unlock_mutex = unlock_mutex,
    .create_semaphore = create_sem,
    .destroy_semaphore = destroy_sem,
    .give_semaphore = give_sem,
};
static const h2_pal_sync_api_t kSync = {.vtable = &kSyncVtable};

/* ---- fake ports -------------------------------------------------------- */

typedef struct fake_port {
  h2_pal_result_t next_rc;
  int busy_after; /* WOULD_BLOCK once this many packets were accepted */
  int sent;
  otg_hci_type_t types[32];
  uint8_t first[32];
  size_t lens[32];
} fake_port_t;

static h2_pal_result_t fake_send(void* user, otg_hci_type_t type,
                                 const uint8_t* data, size_t len) {
  fake_port_t* port = user;
  CHECK(g_lock_depth == 0); /* never called with the queue lock held */
  if (port->busy_after >= 0 && port->sent >= port->busy_after) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  if (port->next_rc != H2_PAL_OK) {
    h2_pal_result_t rc = port->next_rc;
    port->next_rc = H2_PAL_OK;
    return rc;
  }
  if (port->sent < 32) {
    port->types[port->sent] = type;
    port->first[port->sent] = len > 0u ? data[0] : 0u;
    port->lens[port->sent] = len;
  }
  port->sent++;
  return H2_PAL_OK;
}

static void deliver(const otg_hci_sink_t* sink, otg_hci_type_t type,
                    uint8_t first, size_t len) {
  uint8_t data[OTG_HCI_PACKET_MAX + 1];
  memset(data, 0, sizeof(data));
  data[0] = first;
  sink->receive(sink->user, type, data, len);
}

static otg_ble_t g_app;
static h2_runtime_t g_runtime; /* borrowed by g_app for every test */

static void test_relay_and_order(void) {
  g_runtime.sync = &kSync;
  CHECK(otg_ble_init(&g_app, &g_runtime) == H2_PAL_OK);

  fake_port_t controller = {.busy_after = -1};
  fake_port_t host = {.busy_after = -1};
  const otg_hci_port_t controller_port = {.user = &controller,
                                          .send = fake_send};
  const otg_hci_port_t host_port = {.user = &host, .send = fake_send};
  const otg_hci_sink_t* from_host = otg_ble_host_sink(&g_app);
  const otg_hci_sink_t* from_controller = otg_ble_controller_sink(&g_app);

  /* Packets that arrive before the ports are attached stay queued. */
  deliver(from_host, OTG_HCI_COMMAND, 0x03, 3u); /* HCI_Reset */
  CHECK(g_wakes == 1);
  otg_ble_pump(&g_app);
  CHECK(controller.sent == 0);
  otg_ble_attach(&g_app, &controller_port, &host_port);
  deliver(from_host, OTG_HCI_ACL, 0xa1, 9u);
  otg_ble_pump(&g_app);
  CHECK(controller.sent == 2);
  CHECK(controller.types[0] == OTG_HCI_COMMAND && controller.first[0] == 0x03);
  CHECK(controller.types[1] == OTG_HCI_ACL && controller.lens[1] == 9u);

  /* A busy ACL endpoint does not hold back events. */
  host.busy_after = 0;
  deliver(from_controller, OTG_HCI_ACL, 0xb1, 27u);
  otg_ble_pump(&g_app);
  CHECK(host.sent == 0);
  host.busy_after = -1;
  deliver(from_controller, OTG_HCI_EVENT, 0x0e, 6u);
  host.busy_after = 1; /* room for exactly one packet */
  otg_ble_pump(&g_app);
  CHECK(host.sent == 1 && host.types[0] == OTG_HCI_EVENT);
  host.busy_after = -1;
  otg_ble_pump(&g_app);
  CHECK(host.sent == 2 && host.types[1] == OTG_HCI_ACL &&
        host.first[1] == 0xb1);

  otg_ble_stats_t stats = otg_ble_stats(&g_app);
  CHECK(stats.to_controller == 2u && stats.to_host == 2u);
  CHECK(stats.dropped_full == 0u && stats.dropped_rejected == 0u);
}

static void test_drops(void) {
  fake_port_t host = {.busy_after = 0};
  const otg_hci_port_t host_port = {.user = &host, .send = fake_send};
  fake_port_t controller = {.busy_after = -1,
                            .next_rc = H2_PAL_ERR_CLOSED};
  const otg_hci_port_t controller_port = {.user = &controller,
                                          .send = fake_send};
  otg_ble_attach(&g_app, &controller_port, &host_port);
  const otg_hci_sink_t* from_host = otg_ble_host_sink(&g_app);
  const otg_hci_sink_t* from_controller = otg_ble_controller_sink(&g_app);

  for (unsigned i = 0; i < OTG_BLE_TO_HOST_EVENT_DEPTH + 2u; i++) {
    deliver(from_controller, OTG_HCI_EVENT, (uint8_t)i, 4u);
  }
  deliver(from_controller, OTG_HCI_SCO, 0, 4u);
  deliver(from_host, OTG_HCI_EVENT, 0, 4u);
  deliver(from_host, OTG_HCI_COMMAND, 0, OTG_HCI_PACKET_MAX + 1u);
  deliver(from_host, OTG_HCI_COMMAND, 0x01, 3u); /* port rejects it */
  deliver(from_host, OTG_HCI_COMMAND, 0x02, 3u);
  otg_ble_pump(&g_app);

  otg_ble_stats_t stats = otg_ble_stats(&g_app);
  CHECK(stats.dropped_full == 2u);
  CHECK(stats.dropped_unsupported == 2u);
  CHECK(stats.dropped_rejected == 2u); /* oversize + CLOSED */
  CHECK(controller.sent == 1 && controller.first[0] == 0x02);

  host.busy_after = -1;
  otg_ble_pump(&g_app);
  CHECK(host.sent == (int)OTG_BLE_TO_HOST_EVENT_DEPTH);
  CHECK(host.first[0] == 0 && host.first[15] == 15);
}

int main(void) {
  test_relay_and_order();
  test_drops();
  otg_ble_deinit(&g_app);
  if (failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
