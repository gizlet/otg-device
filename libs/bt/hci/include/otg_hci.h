#ifndef OTG_HCI_H
#define OTG_HCI_H

/*
 * Bluetooth HCI packet types and transport ports shared by OTG Device apps.
 *
 * A port carries complete HCI packets without the H4 type byte; the type
 * travels separately. Controller-side transports (ESP VHCI) and host-side
 * transports (USB Bluetooth class) both implement otg_hci_port_t and report
 * received packets through otg_hci_sink_t.
 */

#include "core/h2_pal_errors.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* H4 packet indicators (Bluetooth Core, Vol 4, Part A). */
typedef enum otg_hci_type {
  OTG_HCI_COMMAND = 0x01,
  OTG_HCI_ACL = 0x02,
  OTG_HCI_SCO = 0x03,
  OTG_HCI_EVENT = 0x04,
  OTG_HCI_ISO = 0x05,
} otg_hci_type_t;

#define OTG_HCI_COMMAND_HEADER_LEN 3u
#define OTG_HCI_ACL_HEADER_LEN 4u
#define OTG_HCI_EVENT_HEADER_LEN 2u
/* Largest packet carried: a command with 255 parameter bytes (3 + 255). LE
 * ACL payloads are bounded by the controller's LE ACL buffer size, which is
 * at most 251 bytes for LE Data Length Extension controllers. */
#define OTG_HCI_PACKET_MAX 258u
#define OTG_HCI_ACL_PAYLOAD_MAX (OTG_HCI_PACKET_MAX - OTG_HCI_ACL_HEADER_LEN)

/**
 * Outgoing side of a transport.
 *
 * send() delivers one complete packet. It returns OK once the transport owns
 * a copy (the caller may reuse @p data), WOULD_BLOCK when the transport is
 * busy and the caller should retry after the matching sink's writable()
 * notification, or another error when the packet was dropped.
 */
typedef struct otg_hci_port {
  void* user;
  h2_pal_result_t (*send)(void* user, otg_hci_type_t type,
                          const uint8_t* data, size_t len);
} otg_hci_port_t;

/**
 * Incoming side of a transport. receive() gets one complete packet, borrowed
 * only for the call. writable() reports that a port that returned
 * WOULD_BLOCK may accept a packet again. Both may run in transport task
 * context and must not block for long.
 */
typedef struct otg_hci_sink {
  void* user;
  void (*receive)(void* user, otg_hci_type_t type, const uint8_t* data,
                  size_t len);
  void (*writable)(void* user);
} otg_hci_sink_t;

static inline h2_pal_result_t otg_hci_port_send(const otg_hci_port_t* port,
                                                otg_hci_type_t type,
                                                const uint8_t* data,
                                                size_t len) {
  if (port == NULL || port->send == NULL || (data == NULL && len > 0u)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return port->send(port->user, type, data, len);
}

/**
 * Reassembles ACL packets that arrive split across transfers, such as USB
 * bulk OUT transactions of 64 bytes. Each complete packet goes to sink.
 */
typedef struct otg_hci_acl_reassembler {
  uint8_t buffer[OTG_HCI_PACKET_MAX];
  size_t len;
  bool discarding;
  size_t discard_left;
  uint32_t dropped;
} otg_hci_acl_reassembler_t;

void otg_hci_acl_reset(otg_hci_acl_reassembler_t* acl);

/**
 * Feed @p len bytes. Packets whose declared length exceeds
 * OTG_HCI_ACL_PAYLOAD_MAX are skipped and counted in dropped.
 */
void otg_hci_acl_feed(otg_hci_acl_reassembler_t* acl, const uint8_t* data,
                      size_t len, const otg_hci_sink_t* sink);

#ifdef __cplusplus
}
#endif

#endif
