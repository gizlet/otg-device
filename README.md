# OTG ESP32

OTG ESP32 is an ESP32-based USB accessory that adds Wi-Fi and Bluetooth Low Energy (BLE) to Linux and Windows hosts. The first milestone is a working wireless accessory. A later milestone will use the board's wireless connectivity to deliver over-the-air (OTA) firmware updates to a connected target device.

## First milestone

| Capability | Goal |
| --- | --- |
| USB | The board enumerates as a USB peripheral on Linux and Windows. USB carries host data and powers the prototype. |
| Wi-Fi | The board joins a 2.4 GHz network in station mode and gives the host IP connectivity through a USB network interface. |
| BLE | A host application can use the board to scan for peripherals, connect to one, and read or write a GATT characteristic. |

The BLE milestone uses application-level access. Integration with the operating system's Bluetooth stack is a separate design option. The prototype will determine the USB interfaces, the host control protocol, and the supported Linux distributions and Windows versions.

## Prototype and validation

The ESP32-S3 is the initial candidate because it combines native USB device support, 2.4 GHz Wi-Fi, and BLE. The planned prototype dependencies are an ESP32-S3 development board with its native USB connection exposed, a USB data cable, ESP-IDF, a 2.4 GHz access point, and a BLE peripheral with a known GATT service.

On each host OS, verify USB enumeration, reachability of a test endpoint over Wi-Fi, and a BLE scan, connection, and GATT read or write. Repeat the checks after unplugging and reconnecting the board. Record the OS versions, USB driver requirements, and behavior when Wi-Fi and BLE are active together.

## Later milestone: target OTA

Use the board to receive a firmware image over Wi-Fi and deliver it to the connected target. Prototyping will establish the target connector and transport, image verification, progress reporting, and recovery after an interrupted update.

## Repository status

`README.md` records the project scope and validation goals. Firmware, board files, and host tooling are planned. Build and flash instructions will follow the first runnable prototype.
