# Quectel LG290P board TODO

A candidate fourth board: a Quectel LG290P receiver with an ESP32-S3 on one board.
Nothing is implemented and no board is in hand. This file records what public sources
say, what the port would take, and the questions that need a board or another source
to settle. Researched 2026-11-07; facts marked "reported" come from users, not from
Quectel.

Back to [development](development.md).

## The board

Sold on AliExpress as "Quectel LG290P GNSS RTK Module Development Board With ESP32
S3": [JINYUSHI store](https://www.aliexpress.com/item/1005012569371215.html),
US$110.52 board-only, US$154.82 or US$188.10 with one of two antennas (2026-11-07).
Other stores sell the same board with the same photos, e.g.
[this one](https://www.aliexpress.com/item/1005012589208124.html) at US$120.29. The
listings have no description text, schematic or pin table. From the seller's labelled
photos:

- The PCB silkscreen reads `MOZIHAO`, a maker that also sells LG290P-only boards.
- ESP32-S3-WROOM-1 (PCB antenna), captioned "ESP32S3N16R8": 16 MB flash, 8 MB PSRAM.
- Two USB-C ports: "LG290P UART2" and "ESP32S3 USB&&UART". The second label is the one
  the seller of our UM982 board uses (see [hardware](hardware.md#the-two-usb-c-ports)).
- A 6-pin connector labelled "LG290P UART3" (back side: 5V, GND, TXD3, RXD3, EVENT,
  PPS). Several photos show a 6-wire cable from it to loose dupont plugs at the ESP32
  pin headers.
- SMA and IPEX antenna connectors, ESP32 BOOT and RST buttons, and a separate RST
  button on the receiver side.

Not known: whether the receiver's UART1 is wired to the ESP32 on the board or the
UART3 cable is the only link, and whether the ESP32 can drive the receiver's RESET_N.

Clues, both pointing the same way:

- Our UM982 board ([photo](images/board.png)) has the same layout: receiver at the
  top with a 6-pin connector beside it, a USB-C for the receiver's second port, a
  USB-C for the ESP32, BOT and RST buttons, and the same two pin-header rows. On it the
  receiver's COM1 is on GPIO1 (ESP32 RX) and GPIO2 (TX); our board table has no
  receiver reset pin for it (`components/board/board_table.c`).
- The manual of Mozihao's ESP32-S3 + LC29H board, as quoted on the
  [Quectel forum](https://forums.quectel.com/t/mozihao-esp32-s3-lc29hx-all-in-one-schematic-1pps-reset-to-esp32/61236)
  (2026-09), also puts the receiver on GPIO1 (ESP32 RX) and GPIO2 (TX); the same
  thread asks whether reset reaches the ESP32 and gets no answer.

So the likely wiring is the receiver's UART1 on GPIO1/GPIO2, still to be confirmed.
Mozihao also sells the board on Taobao (shop 合肥墨子号物联网硬件) and has a video of
it ("EPS32S3-RTK LG290P", bilibili BV1HLMy6JEwB).

A different design, the
[7Semi LG290P RTK with ESP32-S3](https://7semi.com/7semi-lg290p-rtk-with-esp32-s3-development-module/)
(US$187), prints its wiring on the back: receiver UART2 to IO12/IO13 and RESET to IO14.

## LG290P compared with the UM980 and UM982

| | LG290P | UM980 | UM982 |
|---|---|---|---|
| Bands | L1, L2, L5, E6, all at once | L1, L2, L5, E6 | L1, L2, L5, E6, set by signal group (`3 6` tracks E6, see [hardware](hardware.md#as-delivered-one-board-october-2026)) |
| GPS | L1 C/A, L2C, L5 | L1 C/A, L1C, L2P(Y), L2C, L5 | same platform as UM980 |
| GLONASS | G1, G2 | G1, G2, G3 | |
| Galileo | E1, E5a, E5b, E6 | E1, E5a, E5b, E6 | |
| BeiDou | B1I, B2I, B3I, B1C, B2a, B2b | B1I, B2I, B3I, B1C, B2a | |
| QZSS, NavIC | QZSS L1, L2C, L5, L6; NavIC L5 | QZSS L1, L2, L5; NavIC L5 | |
| Channels | 1040 | 1408 | 1408 |
| RTK accuracy (datasheet) | 0.8 cm + 1 ppm H, 1.5 cm + 1 ppm V | 0.8 cm + 1 ppm H, 1.5 cm + 1 ppm V | |
| Default UART | 460800 baud | 115200 baud | 115200 baud |
| Commands | Quectel `$PQTM…` NMEA-style sentences | Unicore N4 | Unicore N4 |

LG290P signals are the `PQTMCFGSIGNAL` bits of the protocol specification (all on by
default, L1 cannot be turned off); channels and accuracy are from Quectel's
[specification sheet](https://www.tekmodul.de/wp-content/uploads/2024/06/Quectel_LG290P_GNSS_Specification_V1.0.0_Preliminary_20240227.pdf).
UM980 and UM982 signals are from Onocoy's
[receiver comparison](https://onocoy.com/blog/how-to-choose-a-gnss-base-station-receivers-compared)
(2026-10) and GNSS Store's
[receiver guide](https://gnss.store/blogs/elt-rtk-base/6-choosing-a-receiver). The
LG290P gets Galileo E6 without a signal-group choice. Without GPS L2P(Y) it loses L2 on
the older GPS satellites that send no L2C.

## Onocoy

- Onocoy's [setup page](https://docs.onocoy.com/documentation/3.-become-a-miner/3.-connect-your-station-to-onocoy)
  asks for 1005, 1033, 1077, 1087, 1097, 1117, 1127, 1137 and 1230; MSM4 to MSM7 count,
  once per second. The [quality scale](https://docs.onocoy.com/documentation/mining-rewards-breakdown/quality-scale)
  gives 4 bands a factor of 0.95 (5 bands, with Galileo E5 AltBOC, 1.0); the UM980 is
  capped there too.
- Onocoy does not list the LG290P among its recommended hardware. No public Onocoy
  quality figures for an LG290P station were found.

## Base mode

From the [protocol specification v1.3](https://github.com/sparkfun/SparkFun_RTK_Postcard/raw/main/Firmware/Quectel_LG29xPLGx80P_Series_GNSS_Protocol_Specification_V1.3.pdf)
(v1.2 [from Quectel](https://www.quectel.com/content/uploads/2024/09/Quectel_LG29xPLGx80P_Series_GNSS_Protocol_Specification_V1.2.pdf)),
the [base station application note](https://docs.sparkfun.com/SparkFun_GNSS_LG580P/assets/component_documentation/quectel_lg290p03lgx80p_series_base_station_mode_application_note_v1-1.pdf)
v1.1 and the R02A02S release notes (inside the
[firmware package](https://github.com/sparkfun/SparkFun_RTK_Postcard/tree/main/Firmware)):

- **Mode.** `PQTMCFGRCVRMODE` switches the receiver between its default mode (value 1,
  which Quectel calls "rover"; this file says default mode, so it isn't mixed up with
  the rover project) and base mode (value 2). The module starts in default mode,
  working out its position at 10 Hz and sending NMEA. `PQTMCFGRCVRMODE,W,2` selects
  base mode after `PQTMSAVEPAR` and a restart (`PQTMSRR` or RESET_N). Base mode turns
  NMEA off and sends 1005 and MSM4 once per second; its fix rate is 1 Hz.
- **Position.** `PQTMCFGSVIN` sets survey-in (count, 3D accuracy limit, optional hold
  distance) or a fixed ECEF position; `PQTMSVINSTATUS` reports survey progress. In
  fixed mode `PQTMSVINSTATUS` repeats the configured position.
- **MSM.** `PQTMCFGRTCM` sets one MSM level (3 to 7) for every constellation, an
  elevation threshold and the ephemeris mode. MSM rates use per-constellation names
  (`RTCM3-107X` … `RTCM3-113X`) and share one rate. There is no SBAS MSM.
- **Other messages.** `RTCM3-1005`, `-1006`, `-1033` (added in R01A04S, on by default
  in base mode), `-1230` (added in R02A01S, changed to the standard format in R02A02S;
  its default is not documented) and ephemeris `-1019`, `-1020`, `-1041`, `-1042`,
  `-1044`, `-1046`, set with `PQTMCFGMSGRATE`. There is no 1045. In base mode the fix
  rate is fixed at 1 Hz, so a rate of N means every N seconds.
- **1033 and IDs.** `PQTMCFGANTINF` fills the antenna fields of 1033;
  `PQTMCFGRSID` sets the station ID (0 to 4095).
- **Movement.** `PQTMENV` (added in R01A06S) reports `MovedFlag` (moved since or
  during power-on) and, in base mode, `PosDiff`, "the distance between the current
  position and the fixed coordinates". The specification gives no unit for it.
- **Replies.** `$PQTMxxx,OK[,…]`, or `$PQTMxxx,ERROR,<code>` with 1 invalid
  parameters, 2 failed, 3 unsupported. Standard NMEA XOR checksums. The module prints
  `$PQTMVER` at every start.
- **Saving.** Settings need `PQTMSAVEPAR` and a restart to take effect. No duration is
  documented for the save or the restart; the module prints `$PQTMVER` first at every
  start.
- **Read-back.** Every setting we use has an `R` form whose reply repeats the values.
  `PQTMCFGMSGRATE` reads one message at a time.
- **Baud.** UART1 to UART3 take 9600, 115200, 230400, 460800 and 921600 baud; 460800 is
  the default. When a `PQTMCFGUART` change takes effect is not documented.

Reported:

- With NMEA turned back on in base mode, the position fields stay empty
  ([Quectel forum](https://forums.quectel.com/t/lg290p-questions-about-its-behaviour-in-base-station-mode/38843),
  2024; [rtkbase PR #557](https://github.com/Stefal/rtkbase/pull/557), 2026, firmware
  R01A06S, during survey-in).
- Settings sent after a mode change are not kept until a second save and restart
  ([forum](https://forums.quectel.com/t/msm7-cfgrtcm-not-being-respected-on-lg290p-after-a-restore-unless-its-done-again/39327)).
- Setting an MSM rate puts the MSM level back to 4. Quectel's suggested order is
  `PQTMRESTOREPAR`, `PQTMGNSSSTOP`, rates, `PQTMCFGRTCM` last, save, restart
  ([forum](https://forums.quectel.com/t/lg290p-03-pqtmcfgrtcm-msm-type-7-accepted-but-rtcm-output-stays-msm4-1074-1084-enabling-rtcm3-107x-resets-msm-type-to-4/59370),
  2026, R02A01S). `RTCM3-1077`-style names return `ERROR,1`.
- Survey-in can stall for hours ([rtkbase PR #557](https://github.com/Stefal/rtkbase/pull/557),
  R01A06S, with a 1.2 m accuracy limit). Every stall report found used an accuracy
  limit or old firmware. With no limit (`PQTMCFGSVIN,W,1,60,0,0,0,0`) a survey finished
  on R02A01S: Valid 2, mean accuracy 0.19 m
  ([satpulse](https://github.com/jclark/satpulse) capture `survey.jsonl`, 2026-09).
- No independent position in fixed mode. In the same captures `PQTMPVT` and RMC follow
  the live position during survey-in, then freeze at the surveyed point once it is
  valid. In fixed mode GGA reports quality 7 with the held position
  ([forum](https://forums.quectel.com/t/lg290p-03-pqtmcfgrtcm-msm-type-7-accepted-but-rtcm-output-stays-msm4-1074-1084-enabling-rtcm3-107x-resets-msm-type-to-4/59370),
  R02A01S). `PQTMENV` is the only lead for movement, and no capture with a non-zero
  `PosDiff` has been published.
- On R02A01S, base mode sent 1005, 1033 and MSM4 for all six constellations, and no
  1230, by default (satpulse). Switching to base mode puts the MSM level back to 4;
  MSM7 set after the switch, then saved and restarted, gave 1077, 1087, 1097, 1127
  (satpulse `quectel-config.md`).
- The MSM level can read back as 7 while MSM4 is on the wire (forum thread above), so
  the read-back alone proves nothing.
- `PQTMGNSSSTOP` and `PQTMGNSSSTART` can hang the receiver; a hung LG290P needs a power
  cycle, and a reset command does not bring it back. Echoing the receiver's own output
  back into it made it swallow later commands (satpulse `quectel-config.md`, R02A01S).
- Message rate changes take effect at once; the MSM level, the ephemeris mode, the fix
  rate and the mode change need a save and restart. `PQTMCLRMSG` turns off every
  output on the port (satpulse).
- In default mode RTCM works too: MSM for all six constellations, 1005, 1033 and 1230
  at the fix rate, MSM7 after a save and restart (satpulse).
- The LG290P rejects `PQTMANTENNASTATUS` (the spec lists it for other modules), so
  there is no antenna status (satpulse).
- On R01A04S, `PQTMCFGRTCM` refused an elevation of `-90.0` and took `-90`
  ([forum](https://forums.quectel.com/t/latest-firmware-for-lg290p/58183)).

## Receiver firmware updates

Quectel publishes the protocol: the LG290P(03)&LGx80P(03) Firmware Upgrade Guide
[V1.1](https://raw.githubusercontent.com/sparkfun/SparkFun_GNSS_LG580P/main/docs/assets/component_documentation/quectel_lg290p03lgx80p03_firmware_upgrade_guide_v1-1.pdf)
(2025-07-22; [V1.0](https://docs.sparkfun.com/SparkFun_RTK_Postcard/assets/component_documentation/Quectel_LG290P%2803%29_Firmware_Upgrade_Guide_V1.0.pdf)
2024-07-15).

- **Entry.** "Within 500 ms from power up" the boot loader answers a sync word, sent
  every 20 ms, at 460800 baud. UART1, UART2 and UART3 all support the upgrade. The guide
  does not say whether a RESET_N pulse or `PQTMSRR` opens the window again; for its PC
  tool it asks for a manual reset.
- **Transfer.** Framed packets with a CRC32: firmware info, erase (up to 15 s), 4 KB
  data blocks (500 ms per reply), up to 40 s for the check after the last block (V1.0
  said 15 s), reset. V1.1's example data frame has the length `0x1004`.
- **RESET_N** ([Hardware Design](https://docs.sparkfun.com/SparkFun_LG290P_Quadband_GNSS_RTK_Breakout/assets/component_documentation/quectel_lg290p03_hardware_design_v1-1.pdf)
  V1.1, 2024-12-30): module pin 8, active low, internal 20 kΩ pull-up (no external
  pull-up allowed), held low for at least 100 ms, driven open-collector. Quectel asks
  board makers to bring it out so a stuck module can be reset. The time from release
  to a working UART is not given.
- **Interruption.** If the process fails or stops, no backup firmware runs; the fix is
  to reset and upgrade again.
- **Packages.** One `.pkg` of about 2.7 MB per version, with release notes. The newest
  seen is R02A02S (2026-05-18). Quectel hands firmware out through support; SparkFun
  reposts it, and we keep R01A06S and R02A02S, the same files, in
  [`firmware/lg290p`](../firmware/lg290p/README.md).
- **Without a PC.** SparkFun's MIT-licensed
  [LG290P Arduino library](https://github.com/sparkfun/SparkFun_LG290P_GNSS_Arduino_Library)
  updates from an ESP32. It enters the boot loader with a `PQTMSRR` software reset
  (reported to work on R01A06S and R02A01S); after a failed attempt it falls back to a
  hardware reset.
- **Fit.** Our xmodem flow and `.pkg` checks do not carry over, but the job is the
  same: a staged package, a serial transfer, a version check after.

## What carries over and what does not

Our receiver code is Unicore-only; there is no driver layer a second command set
could plug into.

Carries over: RTCM framing and decoding (`gnss_framing`), `rtcm_hub`, `ntrip_server`,
`local_caster`, `network`, `pkg_store`, and the observations in `gnss_obs`, which come
from standard RTCM (MSM, 1005/1006, 1033) and NMEA (GSV, GGA). Standard NMEA checksums
already fit `$PQTM…` lines. 460800 is already in the baud scan
(`components/um98x_ctl/um98x_plan.c`).

Needs a second implementation or a seam:

1. **Receiver control** (`components/um98x_ctl`): detection through `VERSIONA`,
   configuration (`CONFIG`, `RTCMxxxx COMx`, `MODE BASE`, `SAVECONFIG`), reply
   matching, read-back and restart handling. The LG290P needs its own: identify from
   `$PQTMVER`, apply in the order above, save, restart, confirm.
2. **Message list.** Our profile is a list of RTCM IDs with periods. The LG290P has one
   MSM level for every constellation and per-constellation MSM names.
3. **Signal groups** are Unicore-only (profile, web UI, the UM982 group comparison).
   The LG290P has none.
4. **Movement and relocation checks** (`job_own_fix` in
   `components/station/station.c`, `components/um98x_ctl/um98x_fixcheck.c`) compare
   the receiver's own computed position with the fixed one (`BASEPOSA`, falling back
   to `SPPNAVA`). On the LG290P the NMEA position is reported empty in base mode;
   `PQTMENV` may do the job instead. If it does not, the LG290P has no movement
   report (decision 2).
5. **Receiver firmware updates** (`components/um98x_flash`): a second protocol (above)
   and package check.
6. **Web UI and API**: the receiver page, query allowlist (`VERSIONA`, `CONFIG`,
   `MODE`, `ANT1GROUPA`, `BASEPOSA`, `SPPNAVA`), signal groups and update flow assume
   Unicore (`components/web/ui/js/60-receiver.js`, `components/web/API.md`).
7. **Board entry** (`components/board/board_table.c`): pins, flash size and install
   image, once the wiring is known.

## Implementation plan

Decided 2026-11-07: everything is planned here now and implemented once a board is in
hand. Nothing below is built yet. The open questions are numbered in the next section;
each stage names the ones it waits for.

### Rules for every stage

- **Unicore stays exactly as it is.** The UM982, UM980 and NTRIP-X keep the same
  commands, order, timeouts, stored profile bytes and profile hash. Each stage is
  checked against the existing host tests and QEMU runs, and on all three boards
  before LG290P support is switched on.
- **Every LG290P write is checked.** Settings are read back with the `R` forms after
  the restart, and the RTCM actually arriving must match the profile. An `OK` followed
  by MSM4 on the wire is a failure, not a success.
- **Every wait has a deadline**, and retries are counted.
- **Unproven features ship off or report-only:** movement is report-only, automatic
  relocation and receiver updates are off, until their bench questions are answered.
- **Every receiver firmware version we can use is supported**, with features
  following the version (decision 3 below). R02A02S is the newest and the one to
  test on.
- A CHANGELOG entry under `## Unreleased` for each stage, marked Pending until tested
  on the board; `components/web/API.md` and `tools/bench/tests` with every API change.

### Stage 0: bench day (questions 1 to 8)

Before any firmware, with a serial terminal on the receiver's USB-C ("LG290P UART2")
and a small `uv run` bench script:

1. Board: read the ESP32 module marking; check that receiver UART1 reaches GPIO1/GPIO2
   (the likely wiring), trace UART3 and the receiver's RESET_N (from its RST button)
   to ESP32 GPIOs; find which USB-C is which; try a USB-C-to-C cable.
2. Receiver as delivered: `$PQTMVERNO`, `PQTMCFGUART,R,<1..3>`, `PQTMCFGRCVRMODE,R`,
   and a minute of output per port.
3. Bring the receiver to R02A02S with Quectel's QGNSS on a PC; time each phase. Then,
   from the bench script on the ESP32's UART: does `PQTMSRR` (and RESET_N, if wired)
   open the boot loader's 500 ms window?
4. Base mode: base mode, save, restart; then rates, `PQTMCFGRTCM` last, save, restart,
   without `PQTMGNSSSTOP`. Check the message numbers with
   `tools/bench/rtcm_inspect.py` (1077, not 1074), and the default rates of 1005, 1033
   and 1230. Time `PQTMSAVEPAR` and `PQTMSRR` until `$PQTMVER`.
5. Fixed position with `PQTMENV` at 1 Hz: move the antenna 0.5, 1, 2 and 5 m and
   record `MovedFlag`, `MovingFlag` and `PosDiff` (to find its unit); log GGA,
   `PQTMEPE` and `PQTMPVT`.
6. Survey-in with no accuracy limit for 600 s: does it finish, and how good is the
   mean?
7. `PQTMCFGUART`: when does the receiver switch baud? Only needed if we ever leave
   460800.
8. Default mode with MSM7, for the fallback: does MSM7 come out, and what position
   does its 1005 carry?
9. If the ESP32 can't drive RESET_N: the time from power-on to the ESP32's first UART
   write, to see whether it can catch the boot loader's 500 ms window after a power
   cycle.

### Stage 1: a receiver layer (no LG290P code yet)

- New component `components/receiver_ctl` (`receiver_ctl.h`, `receiver_ctl.c`,
  `receiver_unicore.c`). It sits between `um98x_ctl` (layer 2) and `station`, so the
  layer list in the root `CMakeLists.txt` gets a new layer and `station` moves up one.
- Neutral types in `gnss_types.h`, keeping the `um98x_*` names as aliases at first.
- `station.c` calls the layer instead of `um98x_ctl_*`: `job_detect` to detect,
  `job_identify` to identify, `job_apply` to validate, preview, apply and read back,
  `job_mode` to base state, `job_own_fix` to own position or movement, `job_sky`,
  `job_query`, `job_receiver_reset`. `station.c` keeps the leases, caster pauses,
  maintenance and recovery scheduling.
- Capabilities, which the web UI and API follow: fixed and survey base, own position,
  movement status, automatic relocation, GSV, antenna name, signal groups and their
  comparison, reset pin, receiver updates, supported RTCM messages and rates.
- The Unicore code behind it is not changed. Checked by identical command transcripts
  and profile hashes in host tests, then on the three boards.

### Stage 2: LG290P control (`components/lg290p_ctl`)

- Files after `um98x_ctl`: `lg290p_sentence.c` (builds `$…*XX\r\n` with the XOR
  checksum; refuses `$`, `*` and `,` inside text fields such as the antenna name),
  `lg290p_reply.c` (`OK`, `ERROR,1|2|3`, read replies, `$PQTMVER`, unsolicited lines),
  `lg290p_txn.c`, `lg290p_plan.c`, `lg290p_exec.c`, `lg290p_status.c`. Builder,
  parser and plan are pure code, tested on the host.
- Detection: the saved baud first, then the existing scan (460800 is in it). Unicore
  `VERSIONA` is tried first so Unicore timing does not change; then `$PQTMVERNO`. Only
  a complete reply with a valid checksum counts. The model is stored as `LG290P` with
  the full version in the existing `gnss/model` and `gnss/fw` keys; there is no build
  number. An unknown receiver may stream but gets no configuration.
- Apply, in two phases:
  - A (first setup or an explicit restore): `PQTMRESTOREPAR`, `PQTMCFGRCVRMODE,W,2`,
    `PQTMSAVEPAR`, `PQTMSRR`, identify again.
  - B: `PQTMCLRMSG` (all outputs off on our port), `PQTMCFGSVIN` (fixed position or
    survey), `PQTMCFGRSID`, `PQTMCFGANTINF` (the antenna name, into 1033), GSV and
    status outputs, RTCM rates, `PQTMCFGRTCM` last (elevation written `-90`, not
    `-90.0`), `PQTMSAVEPAR`, `PQTMSRR`, then read everything back. One repeat of phase
    B on a mismatch, then fail without recording the profile as applied.
- Never `PQTMGNSSSTOP` or `PQTMGNSSSTART` in normal setup: they are reported to hang
  the receiver until a power cycle.
- The receiver stays at its default 460800 baud; the station never sends
  `PQTMCFGUART`. A receiver found at another baud by the scan is used at that baud.
- The link never echoes the receiver's output back to it.
- Deadlines, to be tuned on the bench: 3 s quiet after a restart, 3 s per reply, 5 s
  for a save, at most 3 attempts, 20 s for the receiver to come back. `PQTMSRR` has
  no documented reply, so a restart is confirmed by `$PQTMVER` or a `$PQTMVERNO`
  answer within the window. A reset is never sent twice blindly.
- Waits for questions 4, 6 and 10.

### Stage 3: profile and settings

- The stored profile (RTCM IDs with periods), its bytes and its hash stay as they are.
- LG290P checks before saving a profile: one MSM level (3 to 7) and one MSM period for
  every constellation, no SBAS MSM, only 1005, 1006, 1033, 1230 and MSM 107x to 113x.
  The MSM IDs map to `RTCM3-107X` … `RTCM3-113X` plus the level; ephemeris is not
  offered (Onocoy runs its own). Profiles the LG290P can't do are refused, not
  quietly changed.
- Messages follow the receiver's firmware (decision 3): 1033 from R01A04S, 1230 only
  from R02A02S (R02A01S sends it in a non-standard format, so it stays off there). A
  version newer than any we know gets the newest set; the RTCM check confirms it. The
  receiver page says what an older version is missing and offers the update if the
  board supports updates.
- LG290P-only settings (survey accuracy limit and hold distance, MSM elevation
  threshold) go in a new versioned blob in the `gnss` namespace. The Unicore-only
  settings (signal groups, B1C/B2a, MMP, mask) are kept untouched when an LG290P is
  fitted.

### Stage 4: base position and survey

- Survey-in defaults match the Unicore ones (decision 4): the profile's survey time
  (600 s by default, 1 to 3600 s) as the count of 1 Hz positions, no accuracy limit
  (a limit can stop the count, application note FAQ 2) and no hold distance (always
  the new average, like Unicore's distance 0):
  `PQTMCFGSVIN,W,1,<survey_in_s>,0,0,0,0,0`.
- Survey-in is done when `PQTMSVINSTATUS` reports Valid = 2, not when time runs out.
  Then the station locks the position, sets fixed mode and runs its existing check
  that the next 1005 matches to 5 mm.
- Waits for question 6.

### Stage 5: movement

- Parse `PQTMENV` (version 1): `MovedFlag`, `MovingFlag`, `PosDiff`, with its age.
  Show it report-only. It is not fed into the existing movement logic, which needs
  horizontal offsets and uncertainty.
- Question 5 decides the unit, the thresholds and whether it may start a relocation.
  Report-only is acceptable for the first release (decision 2); if `PQTMENV` turns
  out unusable, the LG290P simply has no movement report.
- The fallback, the receiver left in default mode with the station building 1005
  itself (its own position averaging, a 1005 builder, injection into `rtcm_hub`), is
  a separate feature, only if base mode can't keep MSM7 (question 6, decision 1).

### Stage 6: health and recovery

- The recovery ladder (`health_ladder.c`) keeps its order and the Unicore timings.
  The LG290P feeds it reply ages and restart evidence; its uptime is unknown, never
  guessed.
- LG290P reset: `PQTMSRR`, then identify and apply again. RESET_N if the board wires
  it to the ESP32 (question 2): low for at least 100 ms, open-collector, per the
  hardware design. A hung receiver is reported not to answer `PQTMSRR`; without
  RESET_N the station can only report it and ask for a power cycle. No recovery while
  an update owns the link.
- Waits for questions 2 and 21.

### Stage 7: board and install image

- A `board_table.c` entry (`lg290p-mz`) with the measured flash size, pins and reset
  pin, a marked partition table, and an install image in `tools/ci/install_files.sh`
  and the release lists.
- If the only link is the UART3 cable, the existing saved wiring already covers it
  (RX and TX GPIOs, port 3). It has no reset pin. Labels change from COM to UART for
  the LG290P.
- Waits for questions 1 to 3 and 15.

### Stage 8: web UI and API

- The receiver page, preview, queries, signal comparison, sky view and update actions
  follow the capabilities, and the server refuses what a receiver can't do.
- For an LG290P the page shows: firmware version, applied and read-back state, survey
  progress, the RTCM seen, the antenna name, and movement (report-only, unit unknown
  until question 5).
- New API fields for the receiver kind, capabilities, LG290P settings and read-back,
  and movement. Existing fields keep their names and types. The query list is fixed
  per receiver kind, never free text.
- `components/web/API.md`, `tools/bench/tests/test_ui_api.py` and the UI tests.

### Stage 9: receiver firmware updates (`components/lg290p_flash`)

- New component (`lg_proto.c`, `lg_session.c`, `lg_pkg.c`), separate from
  `um98x_flash`. It shares `pkg_store`, the RAW lease from `gnss_link`, the station's
  update lockouts and the update rows in the web UI.
- Steps: check the package, record the job, reset (RESET_N; `PQTMSRR` only if the
  bench shows it opens the window), sync, firmware info, erase, 4 KB blocks, reset
  message, then `$PQTMVERNO`. The running version must equal the package's, or it is
  not a success. Then apply the whole profile again. No `PQTMGNSSSTOP` first: it is
  reported to hang the receiver, and a hung receiver ignores `PQTMSRR`.
- Frames: 4 KB of firmware with the payload length counting the sequence number
  (`0x1004`, as in the V1.1 guide's example), the last frame shorter.
- Deadlines from the V1.1 guide: 500 ms per reply, 15 s for the erase, 40 s for the
  check after the last block. A block is resent, unchanged, only after an error reply, a
  few times at most. After no reply the whole attempt starts again rather than
  resending a block.
- On the bench: send one block twice on purpose and confirm the receiver's check after
  the last block still passes, or fails cleanly (question 25).
- Journal: a new `components/receiver_flash` record type for the LG290P, next to the
  existing Unicore one, which does not change. The point of no return is the erase.
  After a power loss: check whether the new version already runs, otherwise start
  again from block 0, a limited number of times. Every exit puts the baud and the
  line parser back.
- Packages: size within `pkg_store` and a 4 MB cap, the `LG290P03AANRxxAxxS` string
  found in the file and matching the release entry and the receiver, versions
  compared as numbers (R01A06S < R02A01S < R02A02S), unknown names refused. Packages
  come only from our release list, which pins each one by size and SHA-256.
- Release metadata: a separate `lg290p` list in the release manifest. Older stations
  refuse a manifest with more than 4 receiver entries (`components/fw_update/manifest.c`),
  so the existing `gnss` list must not grow. `firmware/release-packages.txt` and
  `tools/release_manifest.py` change with it.
- Offered on a board only if the bench shows the ESP32 can both start an update and
  recover from a failed one there (decision 6); otherwise that board has no receiver
  updates.
- Waits for questions 2, 8 and 25.

### Stage 10: tests

- Host tests: `test/host/main/test_receiver_ctl.c`, `test_lg290p_{sentence,reply,plan,
  exec,status}.c`, `test_lg_{proto,pkg,session,journal}.c`, using the spec's and the
  guide's examples as vectors: broken and split replies, every error code, timeouts,
  both restart phases, read-back mismatches, power loss at each step of an update.
- An LG290P stand-in, `tools/bench/benchlib/lg290p_sim.py`, on the QEMU build's
  receiver link, with a `tools/qemu/e2e.py` run: detection, apply, streaming to the
  fake caster, restart and recovery, and the "`OK` then MSM4" failure.
- The stand-in only checks our code against our reading of the spec; behaviour claims
  come from the bench.

### Stage 11: release

- The README's board table and `docs/hardware.md` get the LG290P board after a
  bench-tested install. An Onocoy test stream for at least 48 h before calling it
  supported.
- Packages go into the release lists when receiver updates are switched on.

### Decisions (2026-11-07)

1. The receiver's own base mode. Default mode with a station-built 1005 only if base
   mode can't keep MSM7 (question 6).
2. Report-only movement is acceptable for the first LG290P release.
3. Support every receiver firmware version we can, with features following the
   version (stage 3).
4. Survey-in defaults as for Unicore (stage 4).
5. A separate `lg290p` list in the release manifest.
6. Receiver updates where the bench shows they work on the board, including recovery
   from a failed update; none where they don't.

## Open questions

The board (needs one in hand or the schematic):

1. Is the receiver's UART1 (or any UART) wired to the ESP32 on the PCB, and on which
   GPIOs? Or is the UART3 cable the only link? Likely UART1 on GPIO1/GPIO2 (see The
   board).
2. Can the ESP32 drive the receiver's RESET_N? Without it, a failed update can only be
   recovered by power-cycling the board or pressing the receiver's RST button, and the
   ESP32 would have to catch the 500 ms window. No clue either way yet.
3. ESP32 module marking (N16R8?), which USB-C goes to which bridge or to the native
   USB, and whether USB-C-to-C cables work (our UM982 board has no CC resistors).
4. Receiver firmware version as delivered, and its settings (mode, baud, messages).

The receiver (bench tests):

5. In fixed-base mode, does `PQTMENV` come out at 1 Hz? What unit is `PosDiff`, and
   do `MovedFlag` and `PosDiff` respond to moving the antenna by 0.5 to 5 m? Captures
   so far show no independent position in fixed mode (see Base mode), so `PQTMENV` is
   the only lead.
6. Does MSM7 stay after the reported order (rates, then `PQTMCFGRTCM` last, save,
   restart) on R02A02S, without `PQTMGNSSSTOP`? Check the message numbers on the wire
   (1077, not 1074). 1033 is on by default in base mode (release notes); is 1230, and
   at what rate? Reported on R02A01S: MSM7 sticks when set after the mode switch,
   1230 is off by default.
7. After `PQTMCFGUART`, does the link change baud at once or after save and restart?
   Only matters if we ever leave 460800; the plan doesn't.
8. Does `PQTMSRR` (or RESET_N) open the boot loader's 500 ms window on the delivered
   firmware, and how long does each update phase take over the ESP32-S3 UART? All
   three receiver UARTs support updates (guide V1.1).

Questions 9 to 14, about how another product drives the LG290P, were answered from
that product's integration; the details stay outside this repository. What they mean
for this port, all still to be confirmed on the bench:

9. **Updates.** Plan on a hardware reset: hold RESET_N low, release it and start
   sending the sync word at once. (`PQTMGNSSSTOP` was sent first there; we leave it
   out, as it is reported to hang the receiver.) This makes board
   question 2 decide whether receiver updates are possible on this board. After a
   failed update, put the UART baud and the line parser back.
10. **Configuring.** Allow about 3 s after a reset before the first command. Writes
    and saves may need repeating: wait for each `OK`, then read the settings back with
    the `R` forms after the restart rather than trusting one write.
11. **MSM7 in default mode.** `$PQTMCFGRTCM,W,7,0,-90,07,06,0,0` (MSM7, no elevation
    limit, no ephemeris) is sent in default mode, but before the MSM rates, the order
    users report puts back to MSM4, and the output is never checked. So it does not
    show that MSM7 works in default mode; question 6 covers it. Nothing shows whether
    1230 or ephemeris work. (Public captures since show MSM7, 1005, 1033 and 1230 in
    default mode after a save and restart; see Base mode.)
12. **Movement.** No use of `PQTMENV`, `PQTMEPE` or `PQTMPVT` was found, so question 5
    stays a bench test.
13. **Baud.** 460800 is kept as the working rate. When the receiver switches after
    `PQTMCFGUART` is still not established (question 7).
14. **Retries.** A few retries per command and per update packet are enough; every
    wait needs a deadline.

Questions 17 to 23 were answered the same way:

17. **End of an update.** Send the boot loader's reset message, then apply the whole
    profile again; don't count on the settings surviving. Then go further than the
    answer did: wait for `$PQTMVER` and compare it with the version string in the
    package before calling the update done.
18. **Update port.** Updates and normal traffic can share one UART. Which receiver UART
    that was is not known, so whether the boot loader answers on UART3 stays a bench
    test (question 8).
19. **Package checks.** Beyond the transfer CRC, check the package ourselves: size and
    the readable `LG290P03AANR…` version string (see
    [`firmware/lg290p`](../firmware/lg290p/README.md)). A model check needs a look at
    the package layout first.
20. **Start-up.** Ask with `$PQTMVERNO` rather than waiting for the `$PQTMVER` start
    line, and accept only a complete reply with a valid checksum. Try the saved baud
    first and keep our baud scan (460800 is in it) as the fallback.
21. **Recovery.** Resetting the receiver after a long UART silence, then configuring
    it again, works. Pause recovery while an update owns the link. Our health ladder
    sets its own timings, to be tuned on the bench.
22. **Other settings.** The answer was a default-mode profile: `PQTMCFGRCVRMODE,W,1`,
    `PQTMCFGRTK`, `PQTMCFGRTCM`, message rates and `PQTMCFGFIXRATE`. Constellations,
    signals, SBAS and masks stay at their defaults. It has no base mode or survey-in, so every
    base-mode step in this file is still ours to prove on the bench.
23. **Status.** Only GGA fix quality and RMC are read; other `$PQTM…` lines are passed
    through unread. Jamming status, `PQTMENV` and the PPS and EVENT pins have no
    example to follow: work from the specification and bench captures. The LG290P
    rejects `PQTMANTENNASTATUS`, so there is no antenna status to show.

Questions 24 and 25 came up while planning the updater and were answered the same way:

24. **Data frame length.** The payload length counts the 4-byte sequence number, so a
    full 4 KB frame says `0x1004`; the V1.0 guide's example showing `0x1000` is the
    inconsistent part, and the V1.1 guide's example shows `0x1004`. Frames of 5 KB (`0x1404`) were also accepted, so the receiver
    reads the length field rather than assuming 4 KB. We send 4 KB frames as the guide
    says.
25. **Resending a block.** An identical frame (same sequence number, data and CRC) is
    resent after an error reply. Nothing shows what happens after no reply at all, or
    whether a block written twice is harmless. The firmware CRC in the info message is
    checked by the receiver after the last block, so a bad write should fail there
    rather than pass. Test it on the bench (stage 9).

For Quectel or the seller:

15. The board's schematic or manual: ask Mozihao (its AliExpress store or the Taobao
    shop), since its ESP32-S3 + LC29H board's manual has a GPIO table.
16. Publishing LG290P packages: decided 2026-11-07, we publish them as SparkFun does.
    They go into `firmware/release-packages.txt` once the station supports the LG290P.
