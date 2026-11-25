#include "otg_bth_in.h"

#include <string.h>

void otg_bth_in_submit(void* param) {
  otg_bth_in_t* ep = param;
  if (atomic_load(&ep->state) != OTG_BTH_IN_QUEUED || !ep->ops->ready()) {
    return; /* suspended: otg_bth_in_resume() submits it */
  }
  if (ep->send(ep->buf, ep->len)) {
    atomic_store(&ep->state, OTG_BTH_IN_INFLIGHT);
  }
}

void otg_bth_in_complete(otg_bth_in_t* ep) {
  atomic_store(&ep->state, OTG_BTH_IN_FREE);
  ep->ops->writable();
}

/* Transfers accepted before a configuration change were abandoned with their
 * endpoints. A queued packet is sent on the new configuration, or dropped
 * when the host has gone. */
void otg_bth_in_reset(otg_bth_in_t* ep, bool configured) {
  /* A fill in progress belongs to the previous host session: cancel it, so
   * the relay's FILLING -> QUEUED step fails and it never queues stale data. */
  int filling = OTG_BTH_IN_FILLING;
  if (atomic_compare_exchange_strong(&ep->state, &filling, OTG_BTH_IN_FREE)) {
    ep->ops->writable();
    return;
  }
  int state = atomic_load(&ep->state);
  if (state == OTG_BTH_IN_INFLIGHT || (!configured && state == OTG_BTH_IN_QUEUED)) {
    atomic_store(&ep->state, OTG_BTH_IN_FREE);
    ep->ops->writable();
  } else if (configured && state == OTG_BTH_IN_QUEUED) {
    otg_bth_in_submit(ep);
  }
}

void otg_bth_in_resume(otg_bth_in_t* ep) { otg_bth_in_submit(ep); }

h2_pal_result_t otg_bth_in_start(otg_bth_in_t* ep, const uint8_t* data,
                                 size_t len) {
  if (len == 0u || len > OTG_HCI_PACKET_MAX || data == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (!ep->ops->mounted()) return H2_PAL_ERR_CLOSED;
  int state = atomic_load(&ep->state);
  if (state == OTG_BTH_IN_QUEUED && ep->ops->ready()) {
    /* Backstop for a refused submission; resume and reconfiguration
     * resubmit on their own. */
    ep->ops->defer(otg_bth_in_submit, ep);
  }
  if (state != OTG_BTH_IN_FREE || !ep->ops->ready()) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  int expected = OTG_BTH_IN_FREE;
  if (!atomic_compare_exchange_strong(&ep->state, &expected,
                                      OTG_BTH_IN_FILLING)) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  memcpy(ep->buf, data, len);
  ep->len = (uint16_t)len;
  expected = OTG_BTH_IN_FILLING;
  if (!atomic_compare_exchange_strong(&ep->state, &expected,
                                      OTG_BTH_IN_QUEUED)) {
    /* A reset cancelled the fill: drop it if the host left, else retry on
     * the new configuration. */
    return ep->ops->mounted() ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_CLOSED;
  }
  ep->ops->defer(otg_bth_in_submit, ep);
  return H2_PAL_OK;
}
