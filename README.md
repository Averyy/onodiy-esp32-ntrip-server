# onocoydiy-esp-um982

Open-source ESP-IDF firmware for the **Unicore UM982 + ESP32-S3** dev board (~C$200)
that turns it into a plug-and-play **Onocoy reference station**: multi-caster NTRIP
output, automatic UM982 configuration, on-device GNSS firmware updates (no Windows
tooling), and firmware updates for both chips from GitHub Releases: the station checks
once a day and shows a banner; nothing is downloaded or installed until you confirm.
All code is original and MIT-licensed.

> Early development. The firmware scaffold builds (component structure, settings, boot
> accounting and safe mode, partition layout, CI, host unit tests), but most subsystems
> are still stubs and nothing has run on hardware yet. Don't flash it to a station you
> rely on.

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
4. **Updates from GitHub Releases** — daily check with a banner in the web UI; the
   ESP32 app (`.bin`) and UM982 firmware (`.pkg`) install only when you click Update
   (or upload a file). Downloads are HTTPS; the `.pkg` lives in
   [`firmware/um982/`](firmware/um982/). Each release carries a signed manifest
   (ECDSA P-256, offline key); the station only offers what it lists and checks every
   download against its size and SHA-256.
5. **Web UI** — Wi-Fi setup, NTRIP configuration, live status, firmware updates, and a
   signal/satellite view.
6. **Health monitoring** — GNSS watchdog, auto-recovery, and data-loss counters.

## Repository layout

| Path | Contents |
|---|---|
| `main/`, `components/` | ESP-IDF firmware (entry point and one component per subsystem) |
| `test/host/` | Host unit tests (ESP-IDF linux target, Unity) |
| `firmware/um982/` | Unicore UM982 receiver firmware the device flashes and offers as a user-confirmed update (see its README) |
| `gnss-splitter/` | KiCad/EasyEDA PCB for an optional GNSS antenna splitter |
| `reference/` | Hardware notes (board, antenna); private research is kept out of the repo |

## Building

Requires ESP-IDF v6.1. From the repository root:

```sh
. $IDF_PATH/export.sh            # your ESP-IDF v6.1 checkout
idf.py set-target esp32s3
idf.py build
idf.py -p <port> flash monitor   # the "ESP32S3 USB&&UART" USB-C port
```

Host unit tests (Linux or macOS, no hardware needed):

```sh
cd test/host
idf.py --preview set-target linux
idf.py build
./build/onocoydiy_host_tests.elf
```

CI runs both for pushes to `main`, release tags and pull requests
(`.github/workflows/build.yml`).

Notes:

- The console runs at 115200 baud on UART0, the same rate as the chip's ROM
  bootloader output.
- Only tag builds in CI are release versions (`vX.Y.Z`, passed as
  `-DRELEASE_VERSION=`; configure refuses it unless HEAD carries that tag). Every
  other build is named `<git describe>+dev` and is never offered updates by the daily
  check. The version is
  fixed when CMake configures; run `idf.py reconfigure` before a local build whose
  version matters.
- The partition table is permanent once a station is deployed (see the comments in
  `partitions.csv`).
- Release manifest: `manifest.json` on each release is
  `{"payload": "<base64 of the manifest JSON>", "signature": "<base64 of the DER
  signature>"}`, signed with `openssl dgst -sha256 -sign key.pem` over the exact
  payload bytes (format in `components/fw_update/priv_include/manifest.h`). Builds
  verify it with the public keys in `CONFIG_FW_UPDATE_MANIFEST_KEY_1`/`_2` (130 hex
  digits, `openssl ec -pubin -in key.pub -text -noout`); a build without a key never
  offers updates. Forks set their own keys.

## Recovery (planned behaviour)

- **Forgot the admin password:** hold the **BOOT** button for 10 s while the station
  is running. The password is cleared and the setup Wi-Fi network (`onocoydiy-XXXX`)
  comes up for 15 minutes so you can set a new one. Casters and settings are kept.
- **Factory reset:** hold **BOOT** for 20 s. Wi-Fi, admin password, casters and
  receiver profile are erased; the station restarts in setup mode. Refused while a
  receiver firmware update is running.
- **Safe mode:** after repeated crashes the station keeps Wi-Fi, the web UI, update
  checks and app installs running but turns off the receiver path and casters. It retries a normal
  boot by itself after 1 h, 4 h and then every 24 h, or immediately from the web UI.
  Brownouts never trigger safe mode; they show a power-supply warning instead.

## License

Our code is under the [MIT License](LICENSE). The firmware image also contains
ESP-IDF, FreeRTOS, lwIP, Mbed TLS, cJSON and other components under their own licenses;
see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md), which ships with every build.
The UM982 firmware packages under `firmware/um982/` are produced and owned by Unicore
Communications, redistributed here for convenience, and are **not** covered by this
project's license.
