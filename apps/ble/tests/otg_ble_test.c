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
static int g_space_gives;
static h2_pal_mutex_t* const kMutex = (h2_pal_mutex_t*)&g_lock_depth;
static h2_pal_semaphore_t* const kSem = (h2_pal_semaphore_t*)&g_wakes;
static h2_pal_semaphore_t* const kSpace = (h2_pal_semaphore_t*)&g_space_gives;
/* Runs when the relay waits for space, standing in for the pump task. */
static void (*g_on_space_wait)(void);
static int g_space_waits;

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
  *out = strcmp(config->name, "ble/space") == 0 ? kSpace : kSem;
  return H2_PAL_OK;
}
static h2_pal_result_t destroy_sem(void* user, h2_pal_semaphore_t* sem) {
  (void)user;
  (void)sem;
  return H2_PAL_OK;
}
static h2_pal_result_t give_sem(void* user, h2_pal_semaphore_t* sem) {
  (void)user;
  if (sem == kSpace) {
    g_space_gives++;
  } else {
    g_wakes++;
  }
  return H2_PAL_OK;
}
static h2_pal_result_t take_sem(void* user, h2_pal_semaphore_t* sem,
                                uint32_t timeout_ms) {
  (void)user;
  (void)timeout_ms;
  CHECK(sem == kSpace);
  CHECK(g_lock_depth == 0); /* never waits with the queue lock held */
  g_space_waits++;
  if (g_on_space_wait == NULL || g_space_waits > 100) {
    fprintf(stderr, "relay waited for space with no pump to free it\n");
    exit(EXIT_FAILURE);
  }
  g_on_space_wait();
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
    .take_semaphore = take_sem,
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

  for (unsigned i = 0; i < OTG_BLE_TO_HOST_EVENT_DEPTH; i++) {
    deliver(from_controller, OTG_HCI_EVENT, (uint8_t)i, 4u);
  }
  deliver(from_controller, OTG_HCI_SCO, 0, 4u);
  deliver(from_host, OTG_HCI_EVENT, 0, 4u);
  deliver(from_host, OTG_HCI_COMMAND, 0, OTG_HCI_PACKET_MAX + 1u);
  deliver(from_host, OTG_HCI_COMMAND, 0x01, 3u); /* port rejects it */
  deliver(from_host, OTG_HCI_COMMAND, 0x02, 3u);
  otg_ble_pump(&g_app);

  otg_ble_stats_t stats = otg_ble_stats(&g_app);
  CHECK(stats.dropped_full == 0u);
  CHECK(stats.dropped_unsupported == 2u);
  CHECK(stats.dropped_rejected == 2u); /* oversize + CLOSED */
  CHECK(controller.sent == 1 && controller.first[0] == 0x02);

  host.busy_after = -1;
  otg_ble_pump(&g_app);
  CHECK(host.sent == (int)OTG_BLE_TO_HOST_EVENT_DEPTH);
  CHECK(host.first[0] == 0 && host.first[15] == 15);
}

/* A scan floods advertising reports while the event endpoint is busy; the
 * Command Complete that follows must still reach the host. */
static void test_reports_leave_room_for_flow_control(void) {
  fake_port_t host = {.busy_after = 0};
  const otg_hci_port_t host_port = {.user = &host, .send = fake_send};
  fake_port_t controller = {.busy_after = -1};
  const otg_hci_port_t controller_port = {.user = &controller,
                                          .send = fake_send};
  otg_ble_attach(&g_app, &controller_port, &host_port);
  const otg_hci_sink_t* from_controller = otg_ble_controller_sink(&g_app);
  otg_ble_stats_t before = otg_ble_stats(&g_app);

  uint8_t report[12] = {0x3e, 10, 0x02, 1};
  for (unsigned i = 0; i < 40u; i++) {
    from_controller->receive(from_controller->user, OTG_HCI_EVENT, report,
                             sizeof(report));
  }
  const uint8_t complete[6] = {0x0e, 4, 1, 0x03, 0x0c, 0};
  from_controller->receive(from_controller->user, OTG_HCI_EVENT, complete,
                           sizeof(complete));
  const uint8_t completed_packets[7] = {0x13, 5, 1, 0x40, 0, 1, 0};
  from_controller->receive(from_controller->user, OTG_HCI_EVENT,
                           completed_packets, sizeof(completed_packets));
  otg_ble_pump(&g_app);
  otg_ble_stats_t after = otg_ble_stats(&g_app);
  CHECK(after.dropped_reports - before.dropped_reports ==
        40u - OTG_BLE_ADV_REPORT_MAX);
  CHECK(after.dropped_full == before.dropped_full);

  host.busy_after = -1;
  otg_ble_pump(&g_app);
  CHECK(host.sent == (int)OTG_BLE_ADV_REPORT_MAX + 2);
  CHECK(host.first[OTG_BLE_ADV_REPORT_MAX] == 0x0e);
  CHECK(host.first[OTG_BLE_ADV_REPORT_MAX + 1] == 0x13);

  /* Delivered reports free their slots for the next scan window. */
  host.busy_after = host.sent;
  for (unsigned i = 0; i < OTG_BLE_ADV_REPORT_MAX; i++) {
    from_controller->receive(from_controller->user, OTG_HCI_EVENT, report,
                             sizeof(report));
  }
  CHECK(otg_ble_stats(&g_app).dropped_reports == after.dropped_reports);
  host.busy_after = -1;
  otg_ble_pump(&g_app);
}

/* A burst of controller ACL data while bulk IN stays busy must wait for room,
 * not lose fragments, and the pump must not call the controller meanwhile. */
static fake_port_t* g_burst_host;

static void free_one_host_slot(void) {
  g_burst_host->busy_after = g_burst_host->sent + 1; /* host reads one */
  otg_ble_pump(&g_app);
}

static void test_acl_burst_waits_for_room(void) {
  fake_port_t host = {.busy_after = 0};
  const otg_hci_port_t host_port = {.user = &host, .send = fake_send};
  fake_port_t controller = {.busy_after = -1};
  const otg_hci_port_t controller_port = {.user = &controller,
                                          .send = fake_send};
  otg_ble_attach(&g_app, &controller_port, &host_port);
  g_burst_host = &host;
  g_on_space_wait = free_one_host_slot;
  g_space_waits = 0;
  const otg_hci_sink_t* from_host = otg_ble_host_sink(&g_app);
  const otg_hci_sink_t* from_controller = otg_ble_controller_sink(&g_app);
  otg_ble_stats_t before = otg_ble_stats(&g_app);

  /* A host command queued for the controller stays queued during the wait. */
  deliver(from_host, OTG_HCI_COMMAND, 0x05, 3u);
  const unsigned burst = OTG_BLE_TO_HOST_ACL_DEPTH + 4u;
  for (unsigned i = 0; i < burst; i++) {
    deliver(from_controller, OTG_HCI_ACL, (uint8_t)(0x80 + i), 27u);
  }
  CHECK(g_space_waits == 4);
  otg_ble_stats_t after = otg_ble_stats(&g_app);
  CHECK(after.dropped_full == before.dropped_full);
  CHECK(after.backpressure_waits - before.backpressure_waits == 4u);

  host.busy_after = -1;
  g_on_space_wait = NULL;
  otg_ble_pump(&g_app);
  CHECK(host.sent == (int)burst);
  for (unsigned i = 0; i < burst; i++) {
    CHECK(host.types[i] == OTG_HCI_ACL && host.first[i] == 0x80 + i);
  }
  CHECK(controller.sent == 1 && controller.first[0] == 0x05);

  /* Without a host there is nobody to wait for: the packet is dropped. */
  otg_ble_attach(&g_app, &controller_port, NULL);
  for (unsigned i = 0; i <= OTG_BLE_TO_HOST_ACL_DEPTH; i++) {
    deliver(from_controller, OTG_HCI_ACL, 0x01, 5u);
  }
  CHECK(otg_ble_stats(&g_app).dropped_full == after.dropped_full + 1u);
}

/* A suspended bus refuses IN transfers with WOULD_BLOCK; credit-bearing
 * events and ACL data stay queued and reach the host after resume. */
static void test_suspend_keeps_packets(void) {
  fake_port_t host = {.busy_after = -1};
  const otg_hci_port_t host_port = {.user = &host, .send = fake_send};
  fake_port_t controller = {.busy_after = -1};
  const otg_hci_port_t controller_port = {.user = &controller,
                                          .send = fake_send};
  otg_ble_attach(&g_app, &controller_port, &host_port);
  const otg_hci_sink_t* from_controller = otg_ble_controller_sink(&g_app);
  otg_ble_stats_t before = otg_ble_stats(&g_app);

  host.busy_after = 0; /* suspended */
  const uint8_t complete[6] = {0x0e, 4, 1, 0x03, 0x0c, 0};
  from_controller->receive(from_controller->user, OTG_HCI_EVENT, complete,
                           sizeof(complete));
  const uint8_t completed_packets[7] = {0x13, 5, 1, 0x40, 0, 1, 0};
  from_controller->receive(from_controller->user, OTG_HCI_EVENT,
                           completed_packets, sizeof(completed_packets));
  deliver(from_controller, OTG_HCI_ACL, 0xc1, 27u);
  for (int i = 0; i < 5; i++) otg_ble_pump(&g_app); /* retries while asleep */
  CHECK(host.sent == 0);

  host.busy_after = -1; /* resumed */
  otg_ble_pump(&g_app);
  CHECK(host.sent == 3);
  CHECK(host.first[0] == 0x0e && host.first[1] == 0x13);
  CHECK(host.types[2] == OTG_HCI_ACL && host.first[2] == 0xc1);
  otg_ble_stats_t after = otg_ble_stats(&g_app);
  CHECK(after.dropped_full == before.dropped_full);
  CHECK(after.dropped_rejected == before.dropped_rejected);
}

/* The host stops reading bulk IN, the ACL queue fills and the controller's
 * receive callback waits. A host HCI_Reset must still reach the controller,
 * and it frees the waiter by flushing the stale host-bound packets. */
static fake_port_t* g_reset_controller;
static const otg_hci_sink_t* g_reset_from_host;

static void send_reset_while_waiting(void) {
  const uint8_t reset[3] = {0x03, 0x0c, 0};
  g_reset_from_host->receive(g_reset_from_host->user, OTG_HCI_COMMAND, reset,
                             sizeof(reset));
  otg_ble_pump(&g_app);
  CHECK(g_reset_controller->sent == 1);
  CHECK(g_reset_controller->types[0] == OTG_HCI_COMMAND &&
        g_reset_controller->first[0] == 0x03);
}

static void test_reset_recovers_blocked_host(void) {
  fake_port_t host = {.busy_after = 0}; /* never reads */
  const otg_hci_port_t host_port = {.user = &host, .send = fake_send};
  fake_port_t controller = {.busy_after = -1};
  const otg_hci_port_t controller_port = {.user = &controller,
                                          .send = fake_send};
  otg_ble_attach(&g_app, &controller_port, &host_port);
  g_reset_controller = &controller;
  g_reset_from_host = otg_ble_host_sink(&g_app);
  g_on_space_wait = send_reset_while_waiting;
  g_space_waits = 0;
  const otg_hci_sink_t* from_controller = otg_ble_controller_sink(&g_app);
  otg_ble_stats_t before = otg_ble_stats(&g_app);

  for (unsigned i = 0; i <= OTG_BLE_TO_HOST_ACL_DEPTH; i++) {
    deliver(from_controller, OTG_HCI_ACL, (uint8_t)i, 27u);
  }
  CHECK(g_space_waits == 1); /* the reset ended the only wait */
  otg_ble_stats_t after = otg_ble_stats(&g_app);
  CHECK(after.flushed_on_reset - before.flushed_on_reset ==
        OTG_BLE_TO_HOST_ACL_DEPTH + 1u);
  CHECK(after.dropped_full == before.dropped_full);

  /* The reset's Command Complete, queued after the flush, is delivered. */
  g_on_space_wait = NULL;
  const uint8_t complete[6] = {0x0e, 4, 1, 0x03, 0x0c, 0};
  from_controller->receive(from_controller->user, OTG_HCI_EVENT, complete,
                           sizeof(complete));
  host.busy_after = -1;
  otg_ble_pump(&g_app);
  CHECK(host.sent == 1 && host.first[0] == 0x0e);
}

int main(void) {
  test_relay_and_order();
  test_reset_recovers_blocked_host();
  test_suspend_keeps_packets();
  test_drops();
  test_reports_leave_room_for_flow_control();
  test_acl_burst_waits_for_room();
  otg_ble_deinit(&g_app);
  if (failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
