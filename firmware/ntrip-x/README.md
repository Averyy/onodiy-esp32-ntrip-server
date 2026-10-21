# GNS NTRIP-X stock firmware

The firmware the GNS NTRIP-X base station ships with, kept so a board can go back to
it. The station does not use these files. These are GNS's v0.8.5 release package
(`ntripx-v0.8.5.zip`, files dated 2024-11-18, renamed here). A unit's flash, read on
2026-10-21 (ESP32-S3 revision v0.2, 8 MB flash, 8 MB embedded PSRAM, secure boot and
flash encryption off), holds the same bootloader, partition table and app byte for
byte; its web image differs only by the `.crc` cache files the web server adds at
first start.

| File | Flash offset | Size | SHA-256 |
|---|---|---|---|
| `bootloader.bin` | `0x0` | 20,912 bytes | `ef0b1a149c29fc46f9e6d89dd321a2e3f8125d055c4e65caf21711dde16337e1` |
| `partition-table.bin` | `0x8000` | 3,072 bytes | `d566104894664687f5713871cf3921abbc79da4b0bb39fa9136ab6356fb2be56` |
| `esp32-ntrip-x-v0.8.5.bin` | `0x10000` | 825,952 bytes | `c37b0de75f6ed13d98730eeb2d565c75b19f56c1fa64ada715e414050293d03a` |
| `www.bin` | `0x210000` | 1,048,576 bytes | `bb91677fc195dc52d8c322ff1db0f1d98329fec0154f16035d783f26bd02b052` |

Verify with `shasum -a 256 -c SHA256SUMS`. The app reports project `esp32-ntripx`,
version `v0.8.5`, built 2024-11-18 with ESP-IDF v4.4.6; the bootloader and app images
pass their own SHA-256. The bootloader header sets DIO, 80 MHz, 8 MB.

**Partition table:** `nvs` 0x9000 (24 KB), `phy_init` 0xF000 (4 KB), `factory` app
0x10000 (2 MB), `www` SPIFFS 0x210000 (1 MB), `coredump` 0x310000 (192 KB). A single
factory app: the stock firmware has no over-the-air updates.

**Not included:** `nvs`, `phy_init` and `coredump`, which hold a unit's own settings
(Wi-Fi and NTRIP logins), its radio calibration and memory from a crash; a board
restored from these files starts with factory settings. Also not included: earlier
versions such as v0.8.3, which GNS supplies on request.

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
