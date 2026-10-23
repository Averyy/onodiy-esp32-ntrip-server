# onodiy-esp32-ntrip-server

Open-source ESP-IDF firmware (ESP32-S3, MIT) that turns a Unicore receiver board (UM982,
UM980, NTRIP-X) into an Onocoy NTRIP reference station. One firmware, only the install
image differs. Docs: `README.md`, `docs/`, `components/web/API.md`.

## Goals

- Plug-and-play: antenna, Wi-Fi, Onocoy credentials; the station configures the receiver itself
- Streams RTCM to Onocoy, up to 3 more NTRIP servers, and a local server for the owner's rover
- Web UI (`components/web/ui`) for setup, status, receiver settings, logs, updates
- Health monitoring with automatic recovery
- App updates from GitHub Releases, always user-started, new versions run on trial and revert on failure
- Receiver firmware updates over the station's own link (xmodem), no PC

## Rules

- **Clean room**: all code written from scratch, never copied from other firmware. Read
  `.claude/notes.md` before writing code: it lists what must not appear in committed files
- Never put a station's name, location or LAN address in tracked files
- Never write to a station (upload, config, restart) without the owner's say-so
- Python via `uv run`, never pip
- Every change goes under `## Unreleased` in `CHANGELOG.md`: short and factual
- API changes update `components/web/API.md` and the tests in `tools/bench/tests`
- Docs state verified facts only
