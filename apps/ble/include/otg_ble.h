#ifndef OTG_BLE_H
#define OTG_BLE_H

/*
 * ble: a USB Bluetooth LE adapter.
 *
 * The app relays HCI packets between the board's BLE controller and a host
 * transport. On ESP32-S3 the host transport is the standard USB Bluetooth
 * class, so Linux binds the in-kernel btusb driver and BlueZ sees hci0; no
 * OTG Device driver is needed.
 *
 * Commands and host ACL data are queued for the controller in arrival order.
 * Controller events and ACL data use separate queues because the USB class
 * carries them on separate endpoints; a busy ACL endpoint never delays events.
 *
 * Advertising reports may fill at most OTG_BLE_ADV_REPORT_MAX event slots and
 * are dropped beyond that, as a scan can produce them faster than the host
 * reads them. The remaining slots stay free for events that carry HCI flow
 * control (Command Complete, Command Status, Number Of Completed Packets) and
 * connection state, whose count the host's command and ACL credits bound.
 *
 * Other controller packets are never dropped while a host is attached: when
 * their queue is full, the controller's receive callback waits until the host
 * side drains a slot. The controller keeps further packets in its own buffers
 * meanwhile, and link-layer flow control holds back the peer. Host-to-controller
 * traffic needs no wait: the controller's command and ACL credits bound it,
 * and the pump keeps delivering it while a receive callback waits, so a host
 * can still recover a stalled link.
 *
 * Sending HCI_Reset to the controller discards every packet still queued for
 * the host, including one a receive callback is waiting to queue: a reset
 * invalidates them, and it frees a host that stopped reading.
 */

#include "h2_runtime.h"
#include "otg_hci.h"
#include "os/h2_pal_sync.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTG_BLE_VERSION "0.1.0"
/** Task name declared to the firmware task policy. */
#define OTG_BLE_TASK_NAME "ble/hci"
#define OTG_BLE_TASK_STACK_SIZE 4096u

#define OTG_BLE_TO_CONTROLLER_DEPTH 16u
#define OTG_BLE_TO_HOST_EVENT_DEPTH 32u
#define OTG_BLE_ADV_REPORT_MAX 16u
#define OTG_BLE_TO_HOST_ACL_DEPTH 8u

typedef struct otg_ble_packet {
  otg_hci_type_t type;
  /** An advertising report, counted against the queue's report limit. */
  bool report;
  uint16_t len;
  uint8_t data[OTG_HCI_PACKET_MAX];
} otg_ble_packet_t;

typedef struct otg_ble_queue {
  otg_ble_packet_t* slots;
  size_t capacity;
  size_t head;
  size_t count;
  size_t report_count;
  size_t report_max;
} otg_ble_queue_t;

typedef struct otg_ble_stats {
  uint32_t to_controller;
  uint32_t to_host;
  uint32_t dropped_full;
  /** Controller packets that waited for room in a full host queue. */
  uint32_t backpressure_waits;
  /** Advertising reports dropped at OTG_BLE_ADV_REPORT_MAX. */
  uint32_t dropped_reports;
  uint32_t dropped_rejected;
  uint32_t dropped_unsupported;
  /** Host-bound packets discarded when HCI_Reset was sent. */
  uint32_t flushed_on_reset;
} otg_ble_stats_t;

/**
 * App state. Fields are private; the struct is public only so a firmware
 * target can place it in static storage.
 */
typedef struct otg_ble {
  const h2_runtime_t* runtime;
  const otg_hci_port_t* controller;
  const otg_hci_port_t* host;
  otg_hci_sink_t controller_sink;
  otg_hci_sink_t host_sink;
  h2_pal_mutex_t* lock;
  h2_pal_semaphore_t* wake;
  /** Given whenever the pump frees a queue slot. */
  h2_pal_semaphore_t* space;
  /** Incremented each time host-bound queues are flushed for HCI_Reset. */
  uint32_t flush_generation;

  otg_ble_queue_t to_controller;
  otg_ble_queue_t to_host_event;
  otg_ble_queue_t to_host_acl;
  otg_ble_packet_t to_controller_slots[OTG_BLE_TO_CONTROLLER_DEPTH];
  otg_ble_packet_t to_host_event_slots[OTG_BLE_TO_HOST_EVENT_DEPTH];
  otg_ble_packet_t to_host_acl_slots[OTG_BLE_TO_HOST_ACL_DEPTH];
  otg_ble_stats_t stats;
} otg_ble_t;

/** Create the bridge's lock and wake semaphore from the Runtime. */
h2_pal_result_t otg_ble_init(otg_ble_t* app, const h2_runtime_t* runtime);
void otg_ble_deinit(otg_ble_t* app);

/** Sink to hand to the controller transport (controller → host packets). */
const otg_hci_sink_t* otg_ble_controller_sink(otg_ble_t* app);
/** Sink to hand to the host transport (host → controller packets). */
const otg_hci_sink_t* otg_ble_host_sink(otg_ble_t* app);

/** Attach both ports; packets received earlier stay queued. */
void otg_ble_attach(otg_ble_t* app, const otg_hci_port_t* controller,
                    const otg_hci_port_t* host);

/** Deliver queued packets until each port is empty or busy. */
void otg_ble_pump(otg_ble_t* app);

/** Pump forever, waking on new packets or writable ports. Task body. */
void otg_ble_run(otg_ble_t* app);

/** Snapshot of packet counters. */
otg_ble_stats_t otg_ble_stats(otg_ble_t* app);

#ifdef __cplusplus
}
#endif

#endif
