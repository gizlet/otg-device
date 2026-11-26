#include "otg_hci.h"

#include <string.h>

void otg_hci_acl_reset(otg_hci_acl_reassembler_t* acl) {
  if (acl == NULL) return;
  acl->len = 0u;
  acl->discarding = false;
  acl->discard_left = 0u;
}

static size_t declared_total(const uint8_t* header) {
  return OTG_HCI_ACL_HEADER_LEN + (size_t)(header[2] | (header[3] << 8));
}

void otg_hci_acl_feed(otg_hci_acl_reassembler_t* acl, const uint8_t* data,
                      size_t len, const otg_hci_sink_t* sink) {
  if (acl == NULL || (data == NULL && len > 0u)) return;
  while (len > 0u) {
    if (acl->discarding) {
      size_t skip = len < acl->discard_left ? len : acl->discard_left;
      data += skip;
      len -= skip;
      acl->discard_left -= skip;
      if (acl->discard_left == 0u) acl->discarding = false;
      continue;
    }
    size_t want = OTG_HCI_ACL_HEADER_LEN;
    if (acl->len >= OTG_HCI_ACL_HEADER_LEN) want = declared_total(acl->buffer);
    size_t take = want - acl->len;
    if (take > len) take = len;
    memcpy(acl->buffer + acl->len, data, take);
    acl->len += take;
    data += take;
    len -= take;

    if (acl->len == OTG_HCI_ACL_HEADER_LEN) {
      size_t total = declared_total(acl->buffer);
      if (total > OTG_HCI_PACKET_MAX) {
        acl->dropped++;
        acl->discarding = true;
        acl->discard_left = total - OTG_HCI_ACL_HEADER_LEN;
        acl->len = 0u;
        continue;
      }
    }
    if (acl->len >= OTG_HCI_ACL_HEADER_LEN &&
        acl->len == declared_total(acl->buffer)) {
      if (sink != NULL && sink->receive != NULL) {
        sink->receive(sink->user, OTG_HCI_ACL, acl->buffer, acl->len);
      }
      acl->len = 0u;
    }
  }
}
