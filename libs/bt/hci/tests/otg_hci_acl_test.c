#include "otg_hci.h"

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

static uint8_t g_out[4][OTG_HCI_PACKET_MAX];
static size_t g_out_len[4];
static size_t g_count;

static void receive(void* user, otg_hci_type_t type, const uint8_t* data,
                    size_t len) {
  (void)user;
  CHECK(type == OTG_HCI_ACL);
  if (g_count < 4u) {
    memcpy(g_out[g_count], data, len);
    g_out_len[g_count] = len;
  }
  g_count++;
}

static const otg_hci_sink_t kSink = {.receive = receive};

static size_t make_acl(uint8_t* out, uint16_t handle, size_t payload,
                       uint8_t fill) {
  out[0] = (uint8_t)handle;
  out[1] = (uint8_t)(handle >> 8);
  out[2] = (uint8_t)payload;
  out[3] = (uint8_t)(payload >> 8);
  memset(out + 4, fill, payload);
  return payload + 4u;
}

static void test_chunks(void) {
  otg_hci_acl_reassembler_t acl;
  memset(&acl, 0, sizeof(acl));
  uint8_t packet[OTG_HCI_PACKET_MAX];
  size_t len = make_acl(packet, 0x2001, 251u, 0xa5);
  g_count = 0u;
  /* USB full-speed bulk OUT delivers 64-byte pieces. */
  for (size_t off = 0; off < len; off += 64u) {
    size_t n = len - off < 64u ? len - off : 64u;
    otg_hci_acl_feed(&acl, packet + off, n, &kSink);
  }
  CHECK(g_count == 1u);
  CHECK(g_out_len[0] == len && memcmp(g_out[0], packet, len) == 0);
}

static void test_header_split_and_back_to_back(void) {
  otg_hci_acl_reassembler_t acl;
  memset(&acl, 0, sizeof(acl));
  uint8_t stream[64];
  size_t a = make_acl(stream, 0x0040, 3u, 0x11);
  size_t b = make_acl(stream + a, 0x0041, 0u, 0);
  g_count = 0u;
  otg_hci_acl_feed(&acl, stream, 2u, &kSink);
  CHECK(g_count == 0u);
  otg_hci_acl_feed(&acl, stream + 2, a + b - 2u, &kSink);
  CHECK(g_count == 2u);
  CHECK(g_out_len[0] == 7u && g_out[0][4] == 0x11);
  CHECK(g_out_len[1] == 4u && g_out[1][0] == 0x41);
}

static void test_oversize_is_skipped(void) {
  otg_hci_acl_reassembler_t acl;
  memset(&acl, 0, sizeof(acl));
  uint8_t big[4 + 300];
  big[0] = 1;
  big[1] = 0;
  big[2] = (uint8_t)300;
  big[3] = (uint8_t)(300 >> 8);
  memset(big + 4, 0xee, 300);
  uint8_t next[8];
  size_t next_len = make_acl(next, 0x0002, 1u, 0x77);
  g_count = 0u;
  otg_hci_acl_feed(&acl, big, 100u, &kSink);
  otg_hci_acl_feed(&acl, big + 100, sizeof(big) - 100u, &kSink);
  otg_hci_acl_feed(&acl, next, next_len, &kSink);
  CHECK(acl.dropped == 1u);
  CHECK(g_count == 1u && g_out_len[0] == 5u && g_out[0][4] == 0x77);

  otg_hci_acl_feed(&acl, big, 50u, &kSink);
  otg_hci_acl_reset(&acl);
  otg_hci_acl_feed(&acl, next, next_len, &kSink);
  CHECK(g_count == 2u);
}

int main(void) {
  test_chunks();
  test_header_split_and_back_to_back();
  test_oversize_is_skipped();
  if (failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
