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
   (or upload a file). The `.pkg` lives in [`firmware/um982/`](firmware/um982/). Each
   release carries a manifest listing every file with its size and SHA-256; the station
   downloads only from this repository's GitHub releases over verified HTTPS and uses a
   file only if it matches (see [Releases and updates](#releases-and-updates)).
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
| `tools/` | Release tooling (`release_manifest.py` writes a release's `manifest.json`) |
| `tools/bench/` | Host-side bench tools for the board: port finder, captures of both serial ports, receiver queries, RTCM inspection, station API dumps, and the receiver boot menu check over its USB port (see its README) |
| `tools/qemu/` | The firmware in Espressif's QEMU, no board needed: build and run script, smoke test (see its README) |
| `sdkconfig.qemu` | QEMU-only configuration overlay used by `tools/qemu/run.sh`; never for a station |
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
(`.github/workflows/build.yml`), together with the bench tools' tests and a QEMU smoke
test (below); a release tag also publishes the GitHub Release (see below).

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
- Receiver firmware updates are off in the default build until they have been tested
  on a bench unit: a package can be uploaded and checked against every rule, but
  nothing is sent to the receiver. A bench build turns them on together with a bench
  console on UART0 that runs the receiver tests one at a time and records every byte
  (`help` lists the commands):

  ```sh
  idf.py -B build-bench -D SDKCONFIG=build-bench/sdkconfig \
         -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.bench" build
  ```

  Never release a bench build: `tools/release_manifest.py` refuses one.

## Developing without hardware (QEMU)

The firmware also runs in Espressif's QEMU, with an emulated Ethernet port in place of
Wi-Fi and UART1 (the receiver link) on a local socket:

```sh
. $IDF_PATH/export.sh
python $IDF_PATH/tools/idf_tools.py install qemu-xtensa   # once, then export.sh again
tools/qemu/run.sh               # builds into build-qemu/, then the web UI is at http://localhost:8080/
nc 127.0.0.1 5556               # in another terminal: the bytes the firmware sends to the receiver
```

Every run starts with empty settings, so the first page asks for the admin password.
`uv run tools/qemu/smoke.py` boots the image headless and checks the web server, the
status API and UART1 in both directions; CI runs it as well. The QEMU build uses
the overlay `sdkconfig.qemu` and is never a station image: configure refuses it for a
release version and the release job refuses to publish it. What differs from a station,
and why UART1 drops bytes while nothing is connected, is in
[`tools/qemu/README.md`](tools/qemu/README.md).

## Releases and updates

**Cutting a release.** Tag the commit and push the tag:

```sh
git tag v0.3.0
git push origin v0.3.0
```

CI builds the tag as release `v0.3.0` (`-DRELEASE_VERSION`), runs the host tests, then
creates the GitHub Release with:

- `onocoydiy-esp-um982.bin`, the app image stations install;
- the receiver package(s) from [`firmware/um982/`](firmware/um982/) (checked against
  its `SHA256SUMS`);
- `manifest.json`, written by [`tools/release_manifest.py`](tools/release_manifest.py):
  the version, and every file with its size and SHA-256;
- `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin` and an `esptool`
  command line in the release notes, for the first flash of a new board, and
  `THIRD_PARTY_NOTICES.md`.

The release is published only once every file is attached. Stations read only the
release GitHub marks Latest; the job gives that mark to the highest `vX.Y.Z`, so a
lower tag pushed later (a fix on an older line) is published without it and offered
to no station. A tag with a suffix (`v0.3.0-rc.1`) becomes a pre-release, which
stations never offer (install it as a file upload). To redo a release, delete it and
its tag on GitHub first. The job needs only `contents: write`. After publishing, it
follows the same download redirects a station does and fails if one breaks a rule
the firmware enforces (skipped while the repository is private).

**What a station trusts.** The repository must be public: stations download without
a token. A release is trusted because of where it comes from:

- every request is HTTPS, with the server certificate checked against the CA bundle and
  the dates of every certificate in its chain checked against the station's clock (no
  update transfer starts before the clock is set);
- it goes only to the releases of the repository set at build time
  (`CONFIG_FW_UPDATE_GITHUB_REPO`, `Averyy/onocoydiy-esp-um982` by default). Every
  redirect must stay on GitHub's release hosts (`github.com`,
  `release-assets.githubusercontent.com`, `objects.githubusercontent.com`), over https,
  without user info, and on `github.com` inside that repository's `/releases/`, or the
  download stops before it connects;
- the manifest names each file with its size and SHA-256, and an app image or receiver
  package is used only if both match (the app image is also checked in flash, and must
  carry the manifest's version and this project's name).

No signing keys, no secure boot. Updates still start only when you click Update, and a
self-built image can always be installed with a file upload.

**Keep the repository's name.** The name is what stations trust. While stations are
in use, never rename or delete the account that owns the repository, and never
rename, transfer or delete the repository. When an account is renamed or deleted,
GitHub lets anyone register its old name. Whoever does, and creates a repository of
the same name, can publish a release that stations offer like any other (built from
this public source), and a station cannot tell the difference. A rename also stops
updates by itself: stations refuse GitHub's redirect to the new name
(`untrusted_host`). To move to another repository:

1. Create the new repository and publish a release there.
2. In the old repository, set `CONFIG_FW_UPDATE_GITHUB_REPO` to the new name and the
   repository variable `RELEASE_MOVE_TO` to the same name, then tag a release. The
   release job publishes that build, which reads the new repository, and refuses one
   that reads any other.
3. Keep the old repository and its account (archive it; never rename or delete it).
   Stations that miss the moving release still read it.

**What this does not cover.** Without a signing key, TLS is all that protects the
manifest on its way from GitHub. Certificate revocation is not checked, and the CA
bundle is fixed in each image, so a root certificate distrusted later stays trusted
until a newer firmware is installed. Whatever CI builds from a tag is what stations
are offered, so the CI build image and actions are part of the trust: the build image
is pinned by digest, the actions only by version tag so far (see the note at the top
of the workflow).

**Forks.** Set `CONFIG_FW_UPDATE_GITHUB_REPO` to your repository; the release job
refuses to publish a build that reads releases from another one. Signing is optional:
put your public key in `CONFIG_FW_UPDATE_MANIFEST_KEY_1` (130 hex digits; `_2` is for
rotating keys) and the private key in the `MANIFEST_SIGNING_KEY` repository secret, and
CI signs `manifest.json` (`{"payload": ..., "signature": ...}`, ECDSA P-256/SHA-256).
A build with a key accepts only manifests signed with it and refuses unsigned ones;
the release job refuses to publish an unsigned manifest for such a build.

```sh
openssl ecparam -name prime256v1 -genkey -noout -out manifest-key.pem   # keep it secret
openssl ec -in manifest-key.pem -pubout | openssl ec -pubin -text -noout  # the pub: hex
```

Manifest format and rules: `components/fw_update/priv_include/manifest.h`.

## Recovery (planned behaviour)

- **Forgot the admin password:** hold the **BOOT** button for 10 s while the station
  is running. The password is cleared and the setup Wi-Fi network (`onocoydiy-XXXX`)
  comes up for 15 minutes (longer while you stay connected to it) so you can set a new
  one. Casters and settings are kept.
- **Wi-Fi gone:** the station never gives up; it keeps retrying (every 30 s at most)
  and after 5 minutes offline also opens the setup Wi-Fi network beside it, so you
  can fix the credentials. That network closes again a minute after the station is
  back online. While the Onocoy stream is down for a network reason on the station's
  side, it reconnects, renews its address and restarts Wi-Fi by itself, and reboots
  for it at most once every 6 hours (never during a receiver firmware update).
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
