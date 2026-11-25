# OTG Device

OTG Device is a GizOS-based USB accessory for Linux and Windows. The first milestone adds Wi-Fi connectivity and application-level Bluetooth Low Energy (BLE) access. A later milestone uses the accessory to deliver over-the-air (OTA) firmware updates to a connected target device.

## First milestone

| Capability | Goal |
| --- | --- |
| USB | The board enumerates as a USB peripheral on Linux and Windows. USB carries host data and powers the prototype. |
| Wi-Fi | The board joins a 2.4 GHz network in station mode. The host recognizes a USB network interface and reaches a test endpoint through the board's Wi-Fi connection. |
| BLE | A host application uses a USB control interface to scan for BLE peripherals, connect, and read or write a GATT characteristic. |

## GizOS and board support

The first target is an ESP32-S3-DevKitC-1 with its native USB port exposed. The firmware will use a pinned GizOS revision and reuse its Wi-Fi and BLE platform interfaces. The host control protocol and OTA workflow will live in shared application code. Each board target will configure its USB controller, radio capabilities, and physical connections. A second board will help establish which USB behavior belongs in a shared interface.

The ESP32-S3 target will prototype the USB Device functions with ESP-IDF and TinyUSB. GizOS's ESP32-S31 CDC serial implementation is a reference for TinyUSB integration. The prototype will evaluate CDC-NCM for the USB network interface, forward host IP traffic over Wi-Fi, and test which Linux distributions and Windows versions can use it. The USB control transport for BLE will be selected during prototyping.

## Prototype and validation

The prototype needs the development board, a USB data cable, a GizOS toolchain with ESP-IDF, a 2.4 GHz Wi-Fi access point, and a BLE peripheral with a known GATT service.

On Linux and Windows, verify that the USB network interface enumerates and that the host reaches a test endpoint over Wi-Fi. Verify a BLE scan, connection, and GATT read or write through the host application. Repeat the checks after reconnecting USB and while Wi-Fi and BLE are active together. Record the OS versions and driver requirements.

## OTA delivery to a connected target

In a later milestone, the accessory will receive a firmware image over Wi-Fi and transfer it to the connected target. The OTA prototype will define the target connector and transport, image verification, progress reporting, and recovery after an interrupted update.

## Repository status

`README.md` records the project scope and validation goals. Firmware, board files, and host tooling are planned. Build and flash instructions will follow the first runnable prototype.
