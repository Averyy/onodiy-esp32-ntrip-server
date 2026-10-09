# onocoydiy-esp-um982

Open-source ESP-IDF firmware for the **Unicore UM982 + ESP32-S3** dev board (~C$200)
that turns it into a plug-and-play **Onocoy reference station**: multi-caster NTRIP
output, automatic UM982 configuration, on-device GNSS firmware updates (no Windows
tooling), and over-the-air updates for both chips from GitHub Releases. All code is
original and MIT-licensed.

> Early development. Hardware is on the way and the firmware isn't written yet — this
> repo currently holds the hardware notes, the GNSS firmware payload, and the splitter
> PCB. Code lands here as the project progresses.

## Why

- Ready-made RTK base stations cost $300+.
- Updating the UM982's own firmware normally requires Windows-only vendor software.
- Existing hobby firmware for these boards doesn't offer turnkey multi-caster
  base-station operation with health monitoring and GNSS firmware updates.

The goal is a turnkey, auditable, free alternative on cheap hardware.

## Target hardware

UM982 + ESP32-S3 dev board (AliExpress, ~C$200):

- ESP32-S3-WROOM-1 N16R8 (16 MB flash, 8 MB PSRAM)
- Unicore UM982 all-band GNSS RTK module
- UM982 COM1 ↔ ESP32 GPIO1 (RX) / GPIO2 (TX)
- UM982 COM2 → USB-C (direct PC access)

Board and pin details: [`reference/um982-board-onocoy.md`](reference/um982-board-onocoy.md).
Antenna and cable notes: [`reference/antenna-specs.md`](reference/antenna-specs.md).

## Features (planned)

1. **Multi-caster NTRIP server** — fan RTCM3 to Onocoy and any other casters from one
   shared ring buffer.
2. **UM982 auto-config** — base mode, the signal group needed for 4-band tracking, and
   the right RTCM messages, applied on first boot.
3. **UM982 firmware update over UART (XMODEM)** from the ESP32 — no Windows or vendor
   tool needed.
4. **OTA via GitHub Releases** — ESP32 app (`.bin`) and UM982 firmware (`.pkg`)
   downloaded over HTTPS; the `.pkg` lives in [`firmware/um982/`](firmware/um982/).
5. **Web UI** — Wi-Fi setup, NTRIP configuration, live status, firmware updates, and a
   signal/satellite view.
6. **Health monitoring** — GNSS watchdog, auto-recovery, and data-loss counters.

## Repository layout

| Path | Contents |
|---|---|
| `firmware/um982/` | Unicore UM982 receiver firmware the device flashes and serves over OTA (see its README) |
| `gnss-splitter/` | KiCad/EasyEDA PCB for an optional GNSS antenna splitter |
| `reference/` | Hardware notes (board, antenna); private research is kept out of the repo |

## Building

ESP-IDF (latest stable), ESP32-S3 target:

```sh
idf.py set-target esp32s3
idf.py build
```

Firmware sources are added as the project progresses.

## License

Our code is under the [MIT License](LICENSE). The UM982 firmware packages under
`firmware/um982/` are produced and owned by Unicore Communications, redistributed here
for convenience, and are **not** covered by this project's license.
