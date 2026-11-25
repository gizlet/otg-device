#ifndef OTG_BTH_IN_H
#define OTG_BTH_IN_H

/*
 * Ownership of one USB IN endpoint buffer (events or ACL data).
 *
 * The relay task only fills a FREE buffer and marks it QUEUED. Every other
 * transition (submit, completion, abandonment on reconfiguration or detach,
 * resubmission on resume) runs in the USB stack's task, so resetting
 * ownership never races a transfer the stack has just accepted. FILLING
 * keeps the USB task from submitting a buffer the relay is still copying.
 *
 * Kept free of TinyUSB so host tests can drive it through otg_bth_in_ops_t.
 */

#include "otg_hci.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { OTG_BTH_IN_FREE, OTG_BTH_IN_FILLING, OTG_BTH_IN_QUEUED, OTG_BTH_IN_INFLIGHT };

typedef struct otg_bth_in_ops {
  /** The host has configured the device. */
  bool (*mounted)(void);
  /** Configured and not suspended: transfers can be submitted. */
  bool (*ready)(void);
  /** Run fn(arg) later in the USB task. */
  void (*defer)(void (*fn)(void*), void* arg);
  /** A buffer became FREE; the relay may send again. */
  void (*writable)(void);
} otg_bth_in_ops_t;

typedef struct otg_bth_in {
  _Atomic int state;
  uint16_t len;
  /** Submit len bytes of buf; false when the stack refuses. */
  bool (*send)(void* buf, uint16_t len);
  const otg_bth_in_ops_t* ops;
  /* Owned by the USB stack from submission until completion. */
  uint8_t buf[OTG_HCI_PACKET_MAX];
} otg_bth_in_t;

/** Relay task: OK once queued, WOULD_BLOCK to retry, CLOSED to drop. */
h2_pal_result_t otg_bth_in_start(otg_bth_in_t* ep, const uint8_t* data,
                                 size_t len);

/* USB task only. */
void otg_bth_in_submit(void* ep);
void otg_bth_in_complete(otg_bth_in_t* ep);
/** The configuration changed (configured) or went away (!configured). */
void otg_bth_in_reset(otg_bth_in_t* ep, bool configured);
/** The bus resumed: submit a buffer queued while suspended. */
void otg_bth_in_resume(otg_bth_in_t* ep);

#ifdef __cplusplus
}
#endif

#endif
