#include "otg_ble.h"

#include <string.h>

/* Bounds the wait so a missed wake-up only delays delivery. */
#define RUN_WAIT_MS 20u
/* Bounds one backpressure wait; the waiter rechecks the queue after it. */
#define SPACE_WAIT_MS 20u

/* ---- queues (caller holds app->lock) ----------------------------------- */

static void queue_init(otg_ble_queue_t* queue, otg_ble_packet_t* slots,
                       size_t capacity, size_t report_max) {
  memset(queue, 0, sizeof(*queue));
  queue->slots = slots;
  queue->capacity = capacity;
  queue->report_max = report_max;
}

static bool queue_push(otg_ble_queue_t* queue, otg_hci_type_t type,
                       bool report, const uint8_t* data, size_t len) {
  if (queue->count == queue->capacity ||
      (report && queue->report_count >= queue->report_max)) {
    return false;
  }
  otg_ble_packet_t* slot =
      &queue->slots[(queue->head + queue->count) % queue->capacity];
  slot->type = type;
  slot->report = report;
  slot->len = (uint16_t)len;
  memcpy(slot->data, data, len);
  queue->count++;
  if (report) queue->report_count++;
  return true;
}

static void queue_pop(otg_ble_queue_t* queue) {
  if (queue->slots[queue->head].report) queue->report_count--;
  queue->head = (queue->head + 1u) % queue->capacity;
  queue->count--;
}

/* LE Meta event carrying an advertising report: legacy (0x02), directed
 * (0x0B) or extended (0x0D). Bluetooth Core, Vol 4, Part E, 7.7.65. */
static bool is_advertising_report(const uint8_t* data, size_t len) {
  if (len < 3u || data[0] != 0x3e) return false;
  return data[2] == 0x02 || data[2] == 0x0b || data[2] == 0x0d;
}

/* ---- sinks -------------------------------------------------------------- */

static void lock(otg_ble_t* app) {
  (void)h2_pal_mutex_lock(app->runtime->sync, app->lock);
}

static void unlock(otg_ble_t* app) {
  (void)h2_pal_mutex_unlock(app->runtime->sync, app->lock);
}

static void wake(void* user) {
  otg_ble_t* app = user;
  (void)h2_pal_semaphore_give(app->runtime->sync, app->wake);
}

static void enqueue(otg_ble_t* app, otg_ble_queue_t* queue,
                    otg_hci_type_t type, bool report, const uint8_t* data,
                    size_t len) {
  if (len > OTG_HCI_PACKET_MAX || (data == NULL && len > 0u)) {
    lock(app);
    app->stats.dropped_rejected++;
    unlock(app);
    return;
  }
  lock(app);
  if (!queue_push(queue, type, report, data, len)) {
    if (report) {
      app->stats.dropped_reports++;
    } else {
      app->stats.dropped_full++;
    }
  }
  unlock(app);
  wake(app);
}

/* Runs in the controller's task. Advertising reports beyond their share are
 * dropped; every other packet waits for room while a host is attached, so the
 * controller's own buffers and link-layer flow control absorb a slow host
 * instead of the relay losing an ACL fragment or a credit-bearing event. The
 * pump does not call into the controller during the wait. */
static void enqueue_from_controller(otg_ble_t* app, otg_ble_queue_t* queue,
                                    otg_hci_type_t type, bool report,
                                    const uint8_t* data, size_t len) {
  if (len > OTG_HCI_PACKET_MAX || (data == NULL && len > 0u)) {
    lock(app);
    app->stats.dropped_rejected++;
    unlock(app);
    return;
  }
  bool waited = false;
  lock(app);
  while (!queue_push(queue, type, report, data, len)) {
    if (report) {
      app->stats.dropped_reports++;
      break;
    }
    if (app->host == NULL) {
      app->stats.dropped_full++;
      break;
    }
    if (!waited) {
      app->stats.backpressure_waits++;
      waited = true;
    }
    app->controller_waiting = true;
    unlock(app);
    wake(app);
    (void)h2_pal_semaphore_take(app->runtime->sync, app->space, SPACE_WAIT_MS);
    lock(app);
  }
  app->controller_waiting = false;
  unlock(app);
  wake(app);
}

static void from_controller(void* user, otg_hci_type_t type,
                            const uint8_t* data, size_t len) {
  otg_ble_t* app = user;
  switch (type) {
    case OTG_HCI_EVENT:
      enqueue_from_controller(
          app, &app->to_host_event, type,
          data != NULL && is_advertising_report(data, len), data, len);
      return;
    case OTG_HCI_ACL:
      enqueue_from_controller(app, &app->to_host_acl, type, false, data, len);
      return;
    default:
      lock(app);
      app->stats.dropped_unsupported++;
      unlock(app);
      return;
  }
}

static void from_host(void* user, otg_hci_type_t type, const uint8_t* data,
                      size_t len) {
  otg_ble_t* app = user;
  if (type != OTG_HCI_COMMAND && type != OTG_HCI_ACL) {
    lock(app);
    app->stats.dropped_unsupported++;
    unlock(app);
    return;
  }
  enqueue(app, &app->to_controller, type, false, data, len);
}

/* ---- pump --------------------------------------------------------------- */

/* Only the pumping task removes packets, so the head slot stays valid while
 * it is being sent with the lock released; producers only append. */
static void drain(otg_ble_t* app, otg_ble_queue_t* queue,
                  const otg_hci_port_t* port, uint32_t* delivered) {
  if (port == NULL) return;
  for (;;) {
    lock(app);
    if (queue->count == 0u) {
      unlock(app);
      return;
    }
    otg_ble_packet_t* packet = &queue->slots[queue->head];
    unlock(app);

    h2_pal_result_t rc =
        otg_hci_port_send(port, packet->type, packet->data, packet->len);
    if (rc == H2_PAL_ERR_WOULD_BLOCK) return;

    lock(app);
    queue_pop(queue);
    if (rc == H2_PAL_OK) {
      (*delivered)++;
    } else {
      app->stats.dropped_rejected++;
    }
    unlock(app);
    (void)h2_pal_semaphore_give(app->runtime->sync, app->space);
  }
}

void otg_ble_pump(otg_ble_t* app) {
  if (app == NULL) return;
  lock(app);
  bool controller_busy = app->controller_waiting;
  unlock(app);
  /* A waiting receive callback holds the controller's task; sending to the
   * controller then could wait on it in turn. */
  if (!controller_busy) {
    drain(app, &app->to_controller, app->controller,
          &app->stats.to_controller);
  }
  drain(app, &app->to_host_event, app->host, &app->stats.to_host);
  drain(app, &app->to_host_acl, app->host, &app->stats.to_host);
}

void otg_ble_run(otg_ble_t* app) {
  for (;;) {
    (void)h2_pal_semaphore_take(app->runtime->sync, app->wake, RUN_WAIT_MS);
    otg_ble_pump(app);
  }
}

/* ---- lifecycle ---------------------------------------------------------- */

h2_pal_result_t otg_ble_init(otg_ble_t* app, const h2_runtime_t* runtime) {
  if (app == NULL || runtime == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(app, 0, sizeof(*app));
  app->runtime = runtime;
  queue_init(&app->to_controller, app->to_controller_slots,
             OTG_BLE_TO_CONTROLLER_DEPTH, 0u);
  queue_init(&app->to_host_event, app->to_host_event_slots,
             OTG_BLE_TO_HOST_EVENT_DEPTH, OTG_BLE_ADV_REPORT_MAX);
  queue_init(&app->to_host_acl, app->to_host_acl_slots,
             OTG_BLE_TO_HOST_ACL_DEPTH, 0u);
  app->controller_sink =
      (otg_hci_sink_t){.user = app, .receive = from_controller, .writable = wake};
  app->host_sink =
      (otg_hci_sink_t){.user = app, .receive = from_host, .writable = wake};

  const h2_pal_mutex_config_t lock_config = {.name = "ble/queue"};
  h2_pal_result_t rc =
      h2_pal_mutex_create(runtime->sync, &lock_config, &app->lock);
  if (rc != H2_PAL_OK) return rc;
  const h2_pal_semaphore_config_t wake_config = {
      .name = "ble/wake", .initial_count = 0u, .max_count = 1u};
  rc = h2_pal_semaphore_create(runtime->sync, &wake_config, &app->wake);
  if (rc == H2_PAL_OK) {
    const h2_pal_semaphore_config_t space_config = {
        .name = "ble/space", .initial_count = 0u, .max_count = 1u};
    rc = h2_pal_semaphore_create(runtime->sync, &space_config, &app->space);
    if (rc != H2_PAL_OK) {
      (void)h2_pal_semaphore_destroy(runtime->sync, app->wake);
      app->wake = NULL;
    }
  }
  if (rc != H2_PAL_OK) {
    (void)h2_pal_mutex_destroy(runtime->sync, app->lock);
    app->lock = NULL;
  }
  return rc;
}

void otg_ble_deinit(otg_ble_t* app) {
  if (app == NULL || app->runtime == NULL) return;
  if (app->space != NULL) {
    (void)h2_pal_semaphore_destroy(app->runtime->sync, app->space);
  }
  if (app->wake != NULL) {
    (void)h2_pal_semaphore_destroy(app->runtime->sync, app->wake);
  }
  if (app->lock != NULL) {
    (void)h2_pal_mutex_destroy(app->runtime->sync, app->lock);
  }
  memset(app, 0, sizeof(*app));
}

const otg_hci_sink_t* otg_ble_controller_sink(otg_ble_t* app) {
  return app != NULL ? &app->controller_sink : NULL;
}

const otg_hci_sink_t* otg_ble_host_sink(otg_ble_t* app) {
  return app != NULL ? &app->host_sink : NULL;
}

void otg_ble_attach(otg_ble_t* app, const otg_hci_port_t* controller,
                    const otg_hci_port_t* host) {
  if (app == NULL) return;
  lock(app);
  app->controller = controller;
  app->host = host;
  unlock(app);
  wake(app);
}

otg_ble_stats_t otg_ble_stats(otg_ble_t* app) {
  otg_ble_stats_t stats;
  memset(&stats, 0, sizeof(stats));
  if (app == NULL) return stats;
  lock(app);
  stats = app->stats;
  unlock(app);
  return stats;
}
