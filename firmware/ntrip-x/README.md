# GNS NTRIP-X stock firmware

The firmware the GNS NTRIP-X base station ships with, kept so a board can go back to
it. The station does not use these files. Read from a unit's flash on 2026-10-21
(ESP32-S3 revision v0.2, 8 MB flash, 8 MB embedded PSRAM, secure boot and flash
encryption off).

| File | Flash offset | Size | SHA-256 |
|---|---|---|---|
| `bootloader.bin` | `0x0` | 20,912 bytes | `ef0b1a149c29fc46f9e6d89dd321a2e3f8125d055c4e65caf21711dde16337e1` |
| `partition-table.bin` | `0x8000` | 4,096 bytes | `945a4a9d4992b5e1adf7b88d98f925a78e77e11569d2eef43169a3cd2e623436` |
| `esp32-ntrip-x-v0.8.5.bin` | `0x10000` | 825,952 bytes | `c37b0de75f6ed13d98730eeb2d565c75b19f56c1fa64ada715e414050293d03a` |
| `www.bin` | `0x210000` | 1,048,576 bytes | `ad3dfdadfc6d344693cb0aa6cfc2a1163894cb0d216f21c4e9df926a3922bf29` |

Verify with `shasum -a 256 -c SHA256SUMS`. The app reports project `esp32-ntripx`,
version `v0.8.5`, built 2024-11-18 with ESP-IDF v4.4.6; the bootloader and app images
pass their own SHA-256. The bootloader header sets DIO, 80 MHz, 8 MB.

**Partition table:** `nvs` 0x9000 (24 KB), `phy_init` 0xF000 (4 KB), `factory` app
0x10000 (2 MB), `www` SPIFFS 0x210000 (1 MB), `coredump` 0x310000 (192 KB). A single
factory app: the stock firmware has no over-the-air updates.

**Not included:** `nvs`, `phy_init` and `coredump`. They hold a unit's own settings
(Wi-Fi and NTRIP logins), its radio calibration and memory from a crash. A board
restored from these files starts with factory settings.

## Restore

Over USB (the board's USB-C, through its FTDI bridge, or the internal debug USB-C):

```
esptool --chip esp32s3 erase-flash
esptool --chip esp32s3 write-flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 esp32-ntrip-x-v0.8.5.bin 0x210000 www.bin
```

Erasing first clears another firmware's settings and update slots. The board then
starts its own hotspot for setup, as a new unit does.

## License

GPLv3, by GNS-Electronics GmbH, based on esp32-xbee by Nebojša Cvetković
([nebkat/esp32-xbee](https://github.com/nebkat/esp32-xbee)), as the firmware's own web
page states. Not covered by this repository's MIT license. Under the GPLv3 the
corresponding source comes from GNS-Electronics (info@gns-electronics.com).
