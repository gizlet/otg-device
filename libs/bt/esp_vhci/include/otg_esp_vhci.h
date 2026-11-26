#ifndef OTG_ESP_VHCI_H
#define OTG_ESP_VHCI_H

/*
 * ESP Bluetooth LE controller as an HCI port, through ESP-IDF's virtual HCI.
 *
 * Requires CONFIG_BT_CONTROLLER_ONLY: no on-chip host stack may own the
 * controller. The host on the other side of the bridge (BlueZ, for example)
 * initializes it with HCI_Reset.
 */

#include "otg_hci.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize NVS (PHY calibration), start the BLE controller and register
 * @p sink for packets from it. @p out_port sends commands and ACL data to the
 * controller; it returns WOULD_BLOCK until the controller can accept a packet,
 * then the sink's writable() fires. Call once; boot-lifetime.
 */
h2_pal_result_t otg_esp_vhci_start(const otg_hci_sink_t* sink,
                                   const otg_hci_port_t** out_port);

#ifdef __cplusplus
}
#endif

#endif
