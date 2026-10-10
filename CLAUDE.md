# onocoydiy-esp-um982

Open source ESP-IDF firmware for UM982 + ESP32-S3 Onocoy reference station.

## Goals

- Multi-caster NTRIP server (fan out RTCM to N casters simultaneously)
- UM982 auto-config on first boot (signal groups, base mode, RTCM messages)
- UM982 firmware updates via xmodem over UART — no Windows/UPrecise needed
- ESP32 + UM982 firmware updates from GitHub Releases: the device checks once a day and shows a web UI banner; every update is started by the user (nothing is downloaded or applied automatically)
- Web UI for config, status, and firmware updates
- Health monitoring with auto-recovery

## Rules

- **Clean room**: all code written from scratch. Do not copy from any existing firmware project
- **Read `.claude/notes.md`** before writing code — it lists what must not appear in committed code
- ESP-IDF (latest stable for ESP32-S3), MIT license
- No comments referencing specific commercial firmware products
