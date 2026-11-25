# OTG Device

OTG Device is open-source firmware that turns MCU development boards into standard USB accessories for Gizlet OS and other hosts. It builds on [GizOS](https://github.com/GizClaw/gizos): portable apps use the GizOS Platform Abstraction Layer (PAL) and Runtime, and each firmware target combines one app with one board.

Every accessory uses a standard USB device class, so the host needs no OTG Device driver. Any supported development board can run any app it has the hardware for.

## Apps

| App | USB class | Host driver | Status |
| --- | --- | --- | --- |
| `ble` | Bluetooth controller (`E0/01/01`) | Linux `btusb`; BlueZ shows `hci0` | ESP32-S3-DevKitC-1 |

`ble` makes the board a USB Bluetooth LE adapter. The board runs only its BLE controller; the host's Bluetooth stack (BlueZ on Linux) drives it over HCI. HCI commands arrive on the control endpoint, events leave on an interrupt endpoint, and ACL data uses a bulk endpoint pair. ESP32-S3 controllers support Bluetooth LE only, not BR/EDR (classic) devices such as headsets.

A Linux host needs `CONFIG_BT` and `CONFIG_BT_HCIBTUSB`. macOS does not use third-party USB Bluetooth controllers, so test there with a raw HCI client over libusb.

## Repository layout

```text
apps/ble/                       portable HCI relay between controller and host transports
libs/bt/hci/                    HCI packet types, transport ports, ACL reassembly
libs/bt/esp_vhci/               ESP-IDF BLE controller through virtual HCI
libs/usb/esp_tinyusb_bth/       ESP-IDF TinyUSB Bluetooth class device
targets/esp_idf_firmware/<app>/<manufacturer>/<board>/
                                firmware entry: board + app + sdkconfig
```

Targets reuse GizOS board packages. A `boards/` directory will be added only for hardware that GizOS does not support. The first directory under `targets/` names the artifact type; these firmware images do not use H2Loader.

## Build and test

Install Bazel 9.2.0, or a Bazelisk launcher that reads `.bazelversion`. `MODULE.bazel` pins GizOS to a specific commit. To develop against a local GizOS checkout, add `common --override_module=gizos=/path/to/gizos` to an untracked `user.bazelrc`.

Run the host unit tests on an Apple silicon Mac:

```sh
bazel test --config=macos_arm64 //apps/... //libs/...
```

On Linux, use `--config=linux_x86_64` or `--config=linux_arm64` instead.

A firmware build needs the ESP-IDF revision pinned by GizOS, which is ESP-IDF 6.2 at `ddb3852`. Set `IDF_PATH`, `IDF_TOOLS_PATH` and `IDF_PYTHON_ENV_PATH`; `bunx @gizclaw/gizos setup esp-idf 6.2` installs that SDK. Then run:

```sh
bazel build --config=esp32s3 //targets/esp_idf_firmware/ble/espressif/esp32s3_devkitc_1:firmware
```

The build writes `combined_factory.bin` and `flasher_args.json` to `bazel-bin/targets/esp_idf_firmware/ble/espressif/esp32s3_devkitc_1/firmware/`.

## ESP32-S3-DevKitC-1

The board has two USB-C ports. The port labeled **USB** connects to the ESP32-S3 native USB controller. While `ble` is running, TinyUSB owns that controller, so the port enumerates as `OTG Device BLE` (`303a:40e0`) rather than the built-in USB Serial/JTAG port. The port labeled **UART** carries the boot console and logs, and it is the usual flashing port.

Flash the combined image through the UART port:

```sh
esptool.py --chip esp32s3 -p <uart-port> write_flash 0x0 combined_factory.bin
```

To flash through the native USB port instead, hold **BOOT**, press **RESET**, and release **BOOT**. The ROM bootloader then exposes its own USB serial port. Press **RESET** after flashing.

The target assumes a DevKitC-1 module with octal PSRAM, which is the GizOS board default. It uses an 8 MB flash layout, which also fits modules with larger flash.

## Validation

On a Linux host:

- `btusb` binds the board and `hciconfig` or `btmgmt info` lists the new controller.
- `bluetoothctl` scans, connects to a BLE peripheral and reads or writes a GATT characteristic.
- The controller reappears after reconnecting USB.

Record the kernel version and any configuration it needs.

## Roadmap

- **Microphone:** a USB Audio Class app on a board with an I2S microphone.
- **More boards:** additional development boards for `ble`, with board packages added here when GizOS does not cover them.

## License

Apache License 2.0. See [LICENSE](LICENSE).
