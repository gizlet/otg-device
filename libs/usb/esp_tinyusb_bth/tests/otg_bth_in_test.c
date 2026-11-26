#include "otg_bth_in.h"

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

/* Fake USB stack: deferred calls run only when the test drains them. */
static bool g_mounted = true;
static bool g_suspended;
static void (*g_deferred_fn)(void*);
static void* g_deferred_arg;
static int g_deferred;
static int g_writable;
static int g_sent;
static uint8_t g_sent_first;

static bool fake_mounted(void) { return g_mounted; }
/* When set, the next ready() check runs a reset first, then reports the
 * readiness observed just before it, as a racing USB task would. */
static otg_bth_in_t* g_reset_in_ready;
static bool g_reset_configured;
static bool fake_ready(void) {
  if (g_reset_in_ready != NULL) {
    otg_bth_in_t* ep = g_reset_in_ready;
    g_reset_in_ready = NULL;
    g_mounted = g_reset_configured;
    otg_bth_in_reset(ep, g_reset_configured);
    return true;
  }
  return g_mounted && !g_suspended;
}
static void fake_defer(void (*fn)(void*), void* arg) {
  g_deferred_fn = fn;
  g_deferred_arg = arg;
  g_deferred++;
}
static void fake_writable(void) { g_writable++; }
static bool fake_send(void* buf, uint16_t len) {
  if (!fake_ready() || len == 0u) return false;
  g_sent++;
  g_sent_first = ((const uint8_t*)buf)[0];
  return true;
}
static void run_deferred(void) {
  if (g_deferred_fn == NULL) return;
  void (*fn)(void*) = g_deferred_fn;
  g_deferred_fn = NULL;
  fn(g_deferred_arg);
}

static const otg_bth_in_ops_t kOps = {.mounted = fake_mounted,
                                      .ready = fake_ready,
                                      .defer = fake_defer,
                                      .writable = fake_writable};

static void reset_fakes(otg_bth_in_t* ep) {
  memset(ep, 0, sizeof(*ep));
  ep->send = fake_send;
  ep->ops = &kOps;
  g_mounted = true;
  g_suspended = false;
  g_deferred_fn = NULL;
  g_deferred = g_writable = g_sent = 0;
}

static const uint8_t kEvent[6] = {0x13, 4, 1, 0x40, 0, 1};

/* The bus suspends after start() accepted a packet but before the USB task
 * submits it; resume must send it without another start(). */
static void test_suspend_between_accept_and_submit(void) {
  otg_bth_in_t ep;
  reset_fakes(&ep);
  CHECK(otg_bth_in_start(&ep, kEvent, sizeof(kEvent)) == H2_PAL_OK);
  g_suspended = true;
  run_deferred();
  CHECK(g_sent == 0 && otg_bth_in_state(&ep) == OTG_BTH_IN_QUEUED);
  CHECK(otg_bth_in_start(&ep, kEvent, sizeof(kEvent)) ==
        H2_PAL_ERR_WOULD_BLOCK);
  g_suspended = false;
  otg_bth_in_resume(&ep);
  CHECK(g_sent == 1 && g_sent_first == 0x13);
  CHECK(otg_bth_in_state(&ep) == OTG_BTH_IN_INFLIGHT);
  otg_bth_in_complete(&ep);
  CHECK(otg_bth_in_state(&ep) == OTG_BTH_IN_FREE && g_writable == 1);
}

static void test_busy_and_closed(void) {
  otg_bth_in_t ep;
  reset_fakes(&ep);
  CHECK(otg_bth_in_start(&ep, kEvent, sizeof(kEvent)) == H2_PAL_OK);
  run_deferred();
  CHECK(otg_bth_in_start(&ep, kEvent, sizeof(kEvent)) ==
        H2_PAL_ERR_WOULD_BLOCK);
  g_mounted = false;
  CHECK(otg_bth_in_start(&ep, kEvent, sizeof(kEvent)) == H2_PAL_ERR_CLOSED);
  CHECK(otg_bth_in_start(&ep, kEvent, 0u) == H2_PAL_ERR_INVALID_ARG);
}

/* Reconfiguration abandons an in-flight transfer and sends a queued one;
 * detach drops a queued one. */
static void test_reconfigure_and_detach(void) {
  otg_bth_in_t ep;
  reset_fakes(&ep);
  CHECK(otg_bth_in_start(&ep, kEvent, sizeof(kEvent)) == H2_PAL_OK);
  run_deferred();
  otg_bth_in_reset(&ep, true);
  CHECK(otg_bth_in_state(&ep) == OTG_BTH_IN_FREE && g_writable == 1);

  CHECK(otg_bth_in_start(&ep, kEvent, sizeof(kEvent)) == H2_PAL_OK);
  otg_bth_in_reset(&ep, true); /* before the deferred submit ran */
  CHECK(g_sent == 2 && otg_bth_in_state(&ep) == OTG_BTH_IN_INFLIGHT);
  run_deferred(); /* stale deferred submit is a no-op */
  CHECK(g_sent == 2);

  otg_bth_in_complete(&ep);
  CHECK(otg_bth_in_start(&ep, kEvent, sizeof(kEvent)) == H2_PAL_OK);
  g_mounted = false;
  otg_bth_in_reset(&ep, false);
  CHECK(otg_bth_in_state(&ep) == OTG_BTH_IN_FREE);
  run_deferred();
  CHECK(g_sent == 2);
}

/* A reset lands after start() read the session and passed its readiness
 * checks, but before it claims the buffer (the interleaving where a reset
 * sees FREE and the relay then claims it). The claim must fail. */
static void test_reset_between_check_and_claim(void) {
  otg_bth_in_t ep;
  reset_fakes(&ep);
  g_reset_in_ready = &ep;
  g_reset_configured = false; /* detach */
  CHECK(otg_bth_in_start(&ep, kEvent, sizeof(kEvent)) == H2_PAL_ERR_CLOSED);
  CHECK(otg_bth_in_state(&ep) == OTG_BTH_IN_FREE);
  g_mounted = true;
  otg_bth_in_reset(&ep, true); /* next host configures the device */
  run_deferred();
  CHECK(g_sent == 0 && otg_bth_in_state(&ep) == OTG_BTH_IN_FREE);

  /* Reconfiguration in the same window: the relay retries on the new one. */
  g_reset_in_ready = &ep;
  g_reset_configured = true;
  CHECK(otg_bth_in_start(&ep, kEvent, sizeof(kEvent)) ==
        H2_PAL_ERR_WOULD_BLOCK);
  CHECK(otg_bth_in_start(&ep, kEvent, sizeof(kEvent)) == H2_PAL_OK);
  run_deferred();
  CHECK(g_sent == 1 && otg_bth_in_state(&ep) == OTG_BTH_IN_INFLIGHT);
}

int main(void) {
  test_reset_between_check_and_claim();
  test_suspend_between_accept_and_submit();
  test_busy_and_closed();
  test_reconfigure_and_detach();
  if (failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
