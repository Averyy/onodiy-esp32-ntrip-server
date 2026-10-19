# UM982 GNSS firmware

Unicore UM982 receiver firmware packages. The ESP32 firmware flashes one of these
to the UM982 over UART (XMODEM), and the same files are published as GitHub Release
assets so a deployed station can offer them as updates, installed only when the user
confirms.

## Current package

| | |
|---|---|
| File | `UM982_R4.10Build18975.pkg` |
| Version | R4.10 Build 18975 |
| Released | 2025-04-30 |
| Model | UM982 (verified in the package header) |
| SHA-256 | `9ac25d1d359e5815b13291f61405faef53d71e306a602184546b32584b35ec71` |

Verify with `shasum -a 256 -c SHA256SUMS`.

**Boards ship with newer firmware.** The UM982 + ESP32-S3 board this project targets
arrived (October 2026) with **R4.10 Build 25164**, compiled 2026-05-21, which is newer
than the package here. The flasher never installs a build lower than the one
running, so this package is of no use on such a receiver. A newer package has to come
from Unicore's UPrecise tool (see below). Receiver flashing from the station stays in
bench builds for now and is lower priority than streaming.

### What's in this build

TruePoint ionosphere-quality service, QZSS L6 MADOCA-PPP, improved PPP fusion and
RTK-assisted PPP convergence, a configurable PVT reliability threshold, smoother
best-position output, anti-jamming tuning, and a GSV message fix (NMEA 4.11 errata).

Galileo E6 tracking (needed for Onocoy's 4-band reward) is available on this
firmware line via `CONFIG SIGNALGROUP 3 6`, according to Unicore's signal-group
tables. Build 18975 itself has not run on our hardware. On the shipped Build 25164,
`CONFIG SIGNALGROUP 3 6` and `7 0` are accepted, and the receiver's `ANT1GROUPA`
query lists Galileo E6 in group `3 6` and BeiDou B2a in group `7 0` (checked without
an antenna). With an antenna, Galileo E6 tracking in group `3 6` is confirmed on
Build 25164.

## Package format

A `.pkg` is the complete, ready-to-flash image, and it is sent to the receiver
byte for byte: the firmware never modifies or repackages it. It does read it first.
Before any byte goes to the receiver, the flasher loads the whole package into RAM
and checks its structure on that copy, which is then the only copy sent:

- the block structure and the block headers;
- the payload sums and block trailers;
- the compressed sections, which are decoded and checked;
- the model string (`UM982` exactly, equal to what the connected receiver reports),
  so a UM980 package is never sent to a UM982 or vice-versa, and the UM982 product
  tags;
- the partition layout, which must be one the firmware knows for that receiver
  build (a package that would change the layout is refused until that path is
  bench-tested);
- size and SHA-256, which must equal the release manifest or what the user
  confirmed for a manual upload; and a build that is not lower than the one running.

The upload page shows what this check reads (model string, layout, build date stamps
of the blocks, size, SHA-256) so you can confirm the file before flashing it.

What the firmware knows about the format comes from examining the Unicore `.pkg`
files themselves, the ones redistributed in this directory. It is PROVISIONAL, to be
confirmed on hardware.

## Updating this directory

1. Obtain a newer `.pkg` for the UM982 (see below).
2. Confirm the model string in the header reads `UM982` and that the file flashes
   cleanly on a bench unit.
3. Replace the file here and regenerate `SHA256SUMS`
   (`shasum -a 256 *.pkg > SHA256SUMS`). The next release tag attaches every `.pkg`
   in this directory to the GitHub Release and lists it in the release manifest
   (the release job refuses a package that does not match `SHA256SUMS`), so stations
   can offer it.

## Where newer firmware comes from

Unicore distributes UM982 firmware through their UPrecise software (Windows), which
performs an online update check against Unicore's servers. The receiver has to be
connected for that check — it signs the request — so pulling the newest build
requires a physical UM982 on the receiver's USB-C. Build 18975 is the latest version
we could obtain without a connected receiver. Now that the board is here (with Build
25164), the next step is to fetch the current package with UPrecise and add it here;
that has not been done yet.

## Source and licensing

These packages are produced and owned by [Unicore Communications](https://en.unicore.com/).
They are redistributed here for the convenience of users flashing their own
receivers, the same way other open hardware projects (e.g. SparkFun's RTK
firmware repositories) mirror them. They are **not** covered by this project's
MIT license. If you intend to redistribute them yourself, confirm the terms with
Unicore.
