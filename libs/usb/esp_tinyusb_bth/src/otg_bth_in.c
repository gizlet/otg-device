#include "otg_bth_in.h"

#include <string.h>

/* ep->state packs the buffer state (low 2 bits) with a session number that
 * every reset increments. The relay's claim (FREE -> FILLING) and publish
 * (FILLING -> QUEUED) are compare-exchanges on the whole word, so any reset
 * after the relay read the word makes them fail: a packet from a previous
 * host session can never be queued, whatever the interleaving. */
#define STATE_MASK 3u
#define SESSION_STEP 4u

static uint32_t pack(uint32_t word, uint32_t state) {
  return (word & ~STATE_MASK) | state;
}

int otg_bth_in_state(otg_bth_in_t* ep) {
  return (int)(atomic_load(&ep->state) & STATE_MASK);
}

void otg_bth_in_submit(void* param) {
  otg_bth_in_t* ep = param;
  uint32_t word = atomic_load(&ep->state);
  if ((word & STATE_MASK) != OTG_BTH_IN_QUEUED || !ep->ops->ready()) {
    return; /* suspended: otg_bth_in_resume() submits it */
  }
  if (ep->send(ep->buf, ep->len)) {
    /* USB task only; the relay never touches a QUEUED word. */
    atomic_store(&ep->state, pack(word, OTG_BTH_IN_INFLIGHT));
  }
}

void otg_bth_in_complete(otg_bth_in_t* ep) {
  atomic_store(&ep->state, pack(atomic_load(&ep->state), OTG_BTH_IN_FREE));
  ep->ops->writable();
}

/* Transfers accepted before a configuration change were abandoned with their
 * endpoints. A queued packet is sent on the new configuration, or dropped
 * when the host has gone. A fill in progress is cancelled either way. */
void otg_bth_in_reset(otg_bth_in_t* ep, bool configured) {
  uint32_t word = atomic_load(&ep->state);
  uint32_t next;
  do {
    uint32_t state = word & STATE_MASK;
    uint32_t keep = configured && state == OTG_BTH_IN_QUEUED
                        ? OTG_BTH_IN_QUEUED
                        : OTG_BTH_IN_FREE;
    next = ((word & ~STATE_MASK) + SESSION_STEP) | keep;
  } while (!atomic_compare_exchange_weak(&ep->state, &word, next));
  if ((next & STATE_MASK) == OTG_BTH_IN_QUEUED) {
    otg_bth_in_submit(ep);
  } else if ((word & STATE_MASK) != OTG_BTH_IN_FREE) {
    ep->ops->writable();
  }
}

void otg_bth_in_resume(otg_bth_in_t* ep) { otg_bth_in_submit(ep); }

h2_pal_result_t otg_bth_in_start(otg_bth_in_t* ep, const uint8_t* data,
                                 size_t len) {
  if (len == 0u || len > OTG_HCI_PACKET_MAX || data == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  /* Read the session before the readiness checks: a reset after this point
   * makes both compare-exchanges below fail. */
  uint32_t word = atomic_load(&ep->state);
  if (!ep->ops->mounted()) return H2_PAL_ERR_CLOSED;
  uint32_t state = word & STATE_MASK;
  if (state == OTG_BTH_IN_QUEUED && ep->ops->ready()) {
    /* Backstop for a refused submission; resume and reconfiguration
     * resubmit on their own. */
    ep->ops->defer(otg_bth_in_submit, ep);
  }
  if (state != OTG_BTH_IN_FREE || !ep->ops->ready()) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  uint32_t filling = pack(word, OTG_BTH_IN_FILLING);
  if (!atomic_compare_exchange_strong(&ep->state, &word, filling)) {
    /* A reset came in: drop if the host left, else retry. */
    return ep->ops->mounted() ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_CLOSED;
  }
  memcpy(ep->buf, data, len);
  ep->len = (uint16_t)len;
  uint32_t expected = filling;
  if (!atomic_compare_exchange_strong(&ep->state, &expected,
                                      pack(filling, OTG_BTH_IN_QUEUED))) {
    return ep->ops->mounted() ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_CLOSED;
  }
  ep->ops->defer(otg_bth_in_submit, ep);
  return H2_PAL_OK;
}
