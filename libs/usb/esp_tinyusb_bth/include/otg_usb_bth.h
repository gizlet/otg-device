#ifndef OTG_USB_BTH_H
#define OTG_USB_BTH_H

/*
 * Standard USB Bluetooth controller on the ESP32-S3 native USB port.
 *
 * The device descriptor uses the Bluetooth class (0xE0/0x01/0x01), so
 * Linux's btusb, and other hosts' generic USB Bluetooth drivers, bind
 * without a vendor driver. HCI commands arrive on the control endpoint,
 * events leave on an interrupt endpoint, and ACL data uses a bulk pair.
 * Interface 1 carries a zero-bandwidth SCO alternate only, because an
 * LE-only controller has no voice channels.
 *
 * Installing TinyUSB moves the USB PHY away from the built-in USB
 * Serial/JTAG controller, so logs must use the UART console.
 */

#include "otg_hci.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Install TinyUSB with the Bluetooth class. Host commands and reassembled
 * ACL packets go to @p sink; writable() fires when an IN endpoint frees up
 * and after the host (re)configures the device. @p out_port sends events and
 * ACL data to the host; it returns CLOSED while the host has not configured
 * the device, and WOULD_BLOCK while the bus is suspended or an endpoint is
 * busy, so suspended packets stay queued until resume. Call once;
 * boot-lifetime.
 */
h2_pal_result_t otg_usb_bth_start(const otg_hci_sink_t* sink,
                                  const otg_hci_port_t** out_port);

#ifdef __cplusplus
}
#endif

#endif
