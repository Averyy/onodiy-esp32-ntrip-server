# UM982 + ESP32-S3 board → onocoy miner

Setup and test notes for the AliExpress UM982 + ESP32-S3 board, used as an onocoy reference station. Written 2026-10-06 before the board arrived. The board description and pin map were corrected on 2026-10-14 from photos of the received board and a first bench session on the powered board (no antenna yet; see **Bench results**). Pick up from **Setup plan**.

## Purchase

| | |
|---|---|
| Listing | https://www.aliexpress.com/item/1005012987845427.html |
| Title | UM982 Full-band GNSS RTK Dual-antenna Positioning And Orientation Module ESP32S3N16R8 WIFI BT BLE 5.0 Development Board |
| Seller | Cmorelink GNSS Store (91.3% positive at time of order) |
| Price | C$200.68 + C$10.26 shipping |
| Variant | "ESP32S3 UM982" (only one) |
| Antenna | Not included. Beitian BT-800S + RG58 TNC-SMA cable, see `antenna-specs.md` |

### Listing details (as seen 2026-10-05)
- Spec fields: Model Name "UM982 ESP32"; "IS It With PCB: Yes"; "GPS type: Receiver & Antenna" (generic field, no antenna shown in the variant); Origin: Mainland China.
- 1 sold, 0 reviews, 3642 in stock.
- Price history: a Google snapshot from Aug 12, 2026 showed this listing at **US$102.04**, so the price swings with promotions.
- Other listings of what looks like the same "UM982 Rover Board/Station + ESP32-S3" (C$217–224): 1005013059873296, 1005013027829503, 1005013036892606, 1005013038169444, 1005013222115077, 1005013190792289. Also 1005013018359434 (C$60.38, probably a partial variant).

### Seller's product description (verbatim points)
1. Main controller ESP32S3N16R8, Wi-Fi + BLE 5.0; supports development in Python, Arduino and ESP-IDF.
2. Onboard UM982 full-frequency positioning and heading RTK module; antenna supports **active SMA (inner hole)** interface.
3. **2× Type-C:** one for ESP32-S3 code download and debugging, the other for **UM982 raw data testing, firmware upgrade and module debugging**.
4. **GH1.25-6P** connector wired independently to **UM982 serial port 3**, for external radios or other devices.
5. **TF card slot**: log RTK module data for later PPK processing.
6. Bluetooth to a phone, or Wi-Fi for data transmission; "CORS account can be used for differential data processing" (sounds like rover/NTRIP-client use, not base/server).
7. Extra ESP32-S3 IO broken out for secondary development.

The seller's PDF "User Manual" is generic safety boilerplate (warnings, "connect antenna, power on, wait for fix", CE mark) with no pinouts or technical content.

## Board layout (checked against the received board, 2026-10)

First written from the seller's low-resolution photos; corrected 2026-10-14 from photos of the received board, front and back, and from the first bench session. **Seen** = visible in those photos. **Bench** = measured on the powered board (2026-10-14, no antenna): confirmed. **Inferred** = deduced, not verified.

Board silkscreen: `RTK_EVB` (seen). About 39 × 91 mm including the ESP32 antenna overhang (inferred, scaled from the header pitch), four brass standoffs. Front, with the SMAs on the left (seen): SMAs, backup cell and UM982 on the left; the unlabelled USB-C, the S3 button and the `PPS` LED on the top edge; the GH connector and the `ESP32S3` USB-C on the bottom edge; header rows P1 (top) and P2 (bottom) and the ESP32-S3 module on the right. The back carries only the TF slot, the CH340N and the GH pin names.

| Part | Detail |
|---|---|
| GNSS module | Unicore **UM982**, front left next to the SMAs (seen). Label P/N **2310415000012** on the received unit too (seen; the receiver's own version reply gives the same P/N, bench), a PN covered by Unicore's UM982 manual R1.8. Firmware as delivered: **R4.10 Build 25164**, compiled 2026-05-21 (bench; see **Bench results**). The seller photo's S/N A2254419032 belongs to another unit; each unit's S/N stays in the private bench log. |
| MCU | **ESP32-S3-WROOM-1**, marking `MCN16R8` (seen): ESP32-S3 revision v0.2, 16 MB flash, 8 MB PSRAM (bench), Wi-Fi + BLE 5 |
| 2× SMA, right-angle edge mount | **J1, lower SMA (GH-connector edge), marked `M`: master. Use this one.** J2, upper SMA, marked `S`: slave (seen). Threaded barrels; female ("inner hole") per the seller. The centre contact is not in the photos, so SMA vs RP-SMA is not confirmed. Each port has its own inductor and diode (L1/D3 for J1, L2/D5 for J2, seen), so both probably carry antenna bias (inferred). |
| USB-C, top edge, **no label** (UM982 side) | UM982 **COM2 → CH340N → USB** (bench: enumerates as 1a86:7523, CH340 family, and answers as the UM982 at 115200; the bridge is the SOP-8 behind it on the back, seen). Opening the port does not reset the receiver (bench). For PC access: UPrecise, firmware upgrade, config, raw data. The seller's name "UM982 UART2" is not printed on the board (seen). |
| USB-C, bottom edge, labelled **`ESP32S3`** | **Orientation-dependent (bench).** One way round it is the ESP32-S3's built-in USB Serial/JTAG (303a:1001, GPIO19/20); with the plug turned over it is a **WCH CH343P** (1a86:55d3) on the ESP32's UART0, with the usual auto-reset transistor pair. The seller's name "ESP32S3 USB&&UART" describes exactly that, but the board prints only `ESP32S3` (seen). esptool works over the built-in USB (bench); the CH343P side should flash too through the auto-reset pair (inferred). The UART0 console needs the CH343P way round. R7/R8 22 Ω beside it (seen). **BOT** + **RST** buttons next to it (seen). |
| GH1.25-6P connector (white, front, bottom edge below the UM982) | UM982 **COM3** (seller; 0 Ω links on TXD3/RXD3 on the back, seen). Pin names on the back silkscreen, from the ESP32 end to the SMA end: **5V, GND, TXD3, RXD3, EVENT, PPS** (seen). The pin-1 dot is at the SMA end (seen), so **pin 1 = PPS, pin 6 = 5V** (inferred). No "UM982 UART3" text on the board (seen). Not needed. |
| TF (microSD) slot | **Back** (seen), push-push, card enters from the top edge; 47 kΩ pull-ups R2–R5, 0 Ω R6, cap C3 (seen). On ESP32 SPI GPIO4–7 per the seller (not traceable on photos; IO4–IO7 are not on P1/P2, which fits). Not needed. |
| 2× pin headers (unpopulated holes, P1/P2) | Front silkscreen, read on the received board (seen, matches the seller photo exactly): **P1** (top edge): GND, IO12, IO11, IO10, IO9, IO8, IO18, IO17, IO16, IO15, 3V3. **P2** (bottom edge): GND, GND, 5V, 5V, IO21, IO14, IO13, IO3, IO2, IO1, 3V3. Square pad = GND at the UM982 end of both rows; no labels on the back. **P2's IO1/IO2 are the UM982 COM1 lines:** two 0 Ω links sit in series between the module's IO1/IO2 pads and the route toward the UM982 (parts seen, route inferred). |
| Buttons | ESP32: **`BOT`** (printed BOT, the boot button, GPIO0) and **`RST`**, between the `ESP32S3` USB-C and the module (seen; BOT's traces run to the module's IO0 corner, inferred pin). UM982 side: **`S3`** with **`RST`** printed below it, top-left between J2 and the `PPS` LED (seen); probably the UM982 reset (inferred, unconfirmed). |
| LEDs | **Only two, both on the front, colour not recorded yet:** `PPS` (top-left, 1 kΩ, driven through a transistor on the receiver side: the UM982's PPS, inferred) and an unlabelled one, D2 (next to the `ESP32S3` USB-C, 1 kΩ: power indicator, inferred). None on the back (seen). No LED traces to an ESP32 GPIO, so the ESP32 has no LED it can drive (inferred), and the station firmware gets no status LED unless D2 turns out to follow ESP32 activity. |
| Backup cell | **BT1**, small coin cell with soldered tabs between the SMAs, with a 1 kΩ resistor and a diode (seen): inferred rechargeable backup for the UM982 (warm starts after a power cycle). |
| Power | **USB-C-to-C cables give no power and no data on either port (bench):** the board leaves out the USB-C CC resistors. Use a USB-A port (a USB-C-to-A adapter or a hub, plus an A-to-C data cable), and power an installed station from a plain 5 V USB-A charger, not a USB-C/PD charger. Two 8-lead ICs with 3.3 V-style feedback dividers and no inductor nearby (seen): inferred LDOs, one per half. `5`-marked diodes by each USB-C (seen): inferred VBUS OR-ing, so either USB-C may power the whole board (unconfirmed). |
| USB chips | **CH340N** (UM982 COM2): SOP-8 on the back, marking illegible; CH340 family by USB ID (bench). **CH343P** (ESP32 UART0): confirmed by USB ID with the `ESP32S3` plug turned over (bench); it is the unmarked QFN-16 next to the module's USB corner (seen), with the 6-lead auto-reset transistor pair beside it (seen; DTR/RTS to EN/IO0, inferred). |

### Seller's pin map

Checked against the photos of the received board and the first bench session (status column).

| ESP32-S3 | Connected to | Status |
|---|---|---|
| **GPIO1** ← | **UM982 TXD1** (ESP RX) | Consistent: a 0 Ω link above P2 `IO1` on the module's IO1 pad (seen); route to the UM982 and direction not visible (inferred) |
| **GPIO2** → | **UM982 RXD1** (ESP TX) | Consistent: a 0 Ω link above P2 `IO2`, trace from the module's IO2 pad to it (seen); direction not visible (inferred) |
| GPIO4 | SD MISO | Not traceable on photos |
| GPIO5 | SD MOSI | Not traceable on photos |
| GPIO6 | SD SCK | Not traceable on photos |
| GPIO7 | SD CS | Not traceable on photos |
| GPIO0 | `BOT` button | Added: traces from BOT to the module's IO0 corner (seen traces, inferred pin) |
| GPIO19/GPIO20 | `ESP32S3` USB-C, one plug orientation (built-in USB D−/D+) | Added (bench: 303a:1001 that way round) |
| GPIO43/GPIO44 (UART0) | CH343P → `ESP32S3` USB-C, plug turned over | Added (bench: 1a86:55d3 that way round, and the stock firmware's UART0 output arrives there; the pins are UART0's defaults, inferred) |
| EN, GPIO0 | CH343P DTR/RTS through the auto-reset transistor pair | Added (pair seen next to the CH343P; wiring inferred) |

| UM982 | Connected to |
|---|---|
| COM1 (TXD1/RXD1) | ESP32-S3 GPIO1/GPIO2 |
| COM2 (TXD2/RXD2) | CH340N → unlabelled top-edge USB-C |
| COM3 (TXD3/RXD3) | GH1.25-6P connector |
| PPS | `PPS` LED through a transistor, and the GH `PPS` pin (both inferred from labels and traces) |

So the data path for onocoy is: **UM982 COM1 → ESP32 (GPIO1) → Wi-Fi → servers.onocoy.com:2101**.

## Bench results (2026-10-14, no antenna)

- **Ports and cables:** see the two USB-C rows above. The `ESP32S3` port depends on the plug orientation (built-in USB or CH343P); on macOS both show up as `/dev/cu.usbmodem*`. C-to-C cables give nothing on either port; USB-A works.
- **Console open/close:** opening and closing the ESP32 console port with `idf.py monitor --no-reset`, serial-mcp or `capture.py --console` does not reset the ESP32. Measured on the built-in USB side; the CH343P side matched another ESP32-S3 board with the same bridge. On the built-in USB a deliberate reset (`capture.py --reset-esp32`, esptool) resets the chip without the port dropping off.
- **Stock ESP32 firmware:** a small Arduino SD-card test sketch from 2022. It prints `error opening test.txt` on UART0 every 400 ms (TF slot empty) and nothing after the ROM lines on the built-in USB; no Wi-Fi, no BLE. 4 MB Arduino partition table (the rest of the 16 MB flash is empty). Secure Boot and Flash Encryption off, download mode on, so an unsigned image flashes as is.
- **ESP32 vs receiver:** the stock firmware sends the receiver nothing and leaves its settings alone, and an ESP32 reset neither resets nor reconfigures the UM982 (its 1 Hz output ran on without a gap).
- **Receiver as delivered:** UM982, firmware **R4.10 Build 25164** (compiled 2026-05-21), newer than the 18975 package in the repository's `firmware/um982/`. Signal group `4 5`, rover mode, COM1/COM2/COM3 at 115200, `RTCMB1CB2A` enabled; COM1 sends a position line and a status report at 1 Hz (the stock setup). Opening COM2 does not reset the receiver.
- **Signal groups on 25164:** it accepts exactly `CONFIG SIGNALGROUP 4 5`, `3 6`, `5 0` and `7 0`. Each switch restarts the receiver in about 9 s, and it answers again about 2 s later; resending the group it already has gives `OK` without a restart. Every other pair tried gets `PARSING FAILED GRAMMAR ERROR,` and no restart. During the restart any command gets `The Board is resetting,do not response any command!` (no leading `$`). Error replies read `PARSING FAILED` (not `FAILD`).
- **Signals per group, no antenna:** `ANT1GROUPA` (sent without a rate; with one it becomes a periodic log) reports the master antenna's tracked signals: `3 6` = Galileo E1/E5a/E5b/E6 (4 bands), `7 0` = all six BeiDou signals including B2a (4 bands), `4 5` and `5 0` at most 3 bands. Whether those signals actually arrive needs sky (to do once the antenna is here).
- **Receiver firmware:** flashing the receiver from the station is now lower priority, since the board ships with newer firmware than the repository's package. The flasher stays bench-only; a newer `.pkg` would come from UPrecise later.

## Why this board / what matters for rewards

### Onocoy quality scale (docs)

`Quality = constellation reward × band reward × signal quality`

- **Constellation reward:** GPS 0.286, Galileo 0.286, BeiDou 0.286, GLONASS 0.142, QZSS 0, NavIC 0.
- **Band reward:** max number of bands on any **one** constellation. 1 band = 0.08, 2 = 0.32, **3 = 0.80**, **4 = 0.95**, 5 = 1.0 (5th = Galileo E5 AltBOC, Septentrio only).
- **Signal quality:** code RMS, phase RMS, cycle slips, sky visibility. Mostly down to antenna and placement.
- Onocoy band columns (from the explorer): **L1** = E1/B1, **L2** = E5b/B2, **L5** = E5a/B2a, **L6** = E6/B3 (LEX).

### UM982 vs UM980 (single antenna)

From Unicore's UM982 User Manual R1.8 (Apr 2025), master-antenna signals:

| Constellation | UM982 master antenna | UM980 |
|---|---|---|
| GPS | L1C/A, L2C/L2P, L5 (3 bands) | + L1C (3 bands) |
| Galileo | E1, E5a, E5b; **E6 only in SIGNALGROUP `3 6`**, on newer firmware (3 or 4 bands) | E1, E5a, E5b, E6 (4 bands) |
| BeiDou | B1I, B2I, B3I in the default group (3 bands); **B2a only in SIGNALGROUP `7 0`** (4 bands) | + B1C, B2a, B2b (4 bands) |
| GLONASS | G1, G2 | G1, G2, G3 |

- **UM982 with 4 bands on one constellation:** **0.95**, same as the UM980.
- **UM982 with 3 bands max** (the default `4 5` group): **0.80**, about 16% less reward.
- **Two ways to 4 bands (0.95)**, from the SIGNALGROUP tables in the *Unicore Reference Commands Manual for N4 High Precision Products V2* R1.4 (Tables 4-31 and 4-32). Changing it reboots the module and saves itself (bench, 25164: about 9 s, see **Bench results**). Leaving out the second parameter sets the slave to 0.

  | `CONFIG SIGNALGROUP` | Master antenna signals | 4-band constellation | Slave antenna |
  |---|---|---|---|
  | `4 5` (default) | BDS B1I/B2I/B3I, GPS L1/L2/L5, GLO G1/G2, GAL E1/E5a/E5b | None, max 3 bands → 0.80 | On |
  | `3 6` | BDS B1I/B3I/B1C/B2b, GPS L1/L2/L5, GLO G1/G2, GAL E1/E5a/E5b/**E6** | **Galileo** | On |
  | `7 0` | BDS B1I/B2I/B3I/B1C/**B2a**/B2b, GPS L1/L2/L5, GLO G1/G2, GAL E1/E5a/E5b | **BeiDou** | Off ("for base station mode only") |
  | `5 0` | Low-power set (bench: no L5 at all, at most 3 bands) | None | Off |

  Bench (25164, `ANT1GROUPA`, no antenna): the master-antenna signals of `4 5`, `3 6` and `7 0` match this table.

- **`3 6` vs `7 0`:** both give 0.95.
  - `3 6` adds Galileo E6 but drops BeiDou B2I and B2a.
  - `7 0` keeps all 6 BeiDou signals, drops E6, and turns the unused slave antenna off.
  - Test both on the bench and keep whichever shows more signals on the onocoy explorer.
- **Firmware:** Unicore added BDS B1C/B2b, Galileo E6 and QZSS L6 to the UM982 spec in R1.8, so older firmware may lack E6, and it's unconfirmed whether older builds support group `7 0`. **The received board's Build 25164 accepts both `3 6` and `7 0` and reports E6 and B2a in them (bench), so it needs no receiver firmware update for 4 bands.** Tracking with sky is still to confirm.

## Safety

- Connect or disconnect the antenna (and cable) **with the board unpowered**, especially a fixed outdoor or roof antenna. Mains adapters can put a stray voltage on the chassis, and that can kill the receiver.
- Powering up with no antenna is fine. Leaving the slave SMA empty permanently is fine.
- Don't short an SMA centre pin to its shell (the ports carry antenna bias voltage; both J1 and J2 have a bias network).
- Use a **plain 5 V USB-A** supply with an A-to-C cable. No fast-charge (QC/PD) or USB-C supplies (a C-to-C cable gives this board no power anyway, bench), and no no-name hubs for the install.

## Setup plan

### Step 0: Unbox and check
- [x] Board matches photos: UM982, ESP32-S3-WROOM-1 (`MCN16R8`), 2× SMA, 2× USB-C, TF slot (back). Extra on the real board: backup cell BT1, two LEDs (`PPS`, D2). The seller's port names are not printed on it.
- [x] Note the UM982 label P/N and S/N. P/N 2310415000012; the S/N is in the private bench log.
- [x] Find the "M" marking by the SMA connectors: **J1, the lower SMA** (GH-connector edge). J2, the upper one, is marked `S`.
- [x] Ports (bench): plug both USB-C ports into USB-A ports (adapter or hub + A-to-C data cable); C-to-C gives nothing. The `ESP32S3` port is the built-in USB one way round and the CH343P (UART0 console) the other.

### Step 1: Check firmware version + E6, on the Mac (no Windows)
1. Antenna on the master SMA (power off first), antenna outside or with good sky view.
2. Plug the **unlabelled USB-C on the top edge** (UM982 side, next to S3 and the `PPS` LED; the seller calls it "UM982 UART2") into a USB-A port on the Mac (USB-C-to-A adapter or hub + A-to-C data cable; C-to-C gives nothing).
3. `ls /dev/cu.*` → expect `/dev/cu.usbserial-XXXX` or `/dev/cu.wchusbserial-XXXX` (macOS has a built-in CH34x driver).
4. `screen /dev/cu.usbserial-XXXX 115200` (quit: Ctrl-A, K, Y). Opening the port does not reset the receiver (bench).
5. Firmware version: type `VERSIONA` + Enter. If no reply, try `LOG VERSIONA ONCE`. As delivered (bench, 2026-10-14): `R4.10Build25164`, compiled 2026-05-21.
6. **Set the signal group first.** The default `4 5` never tracks E6, so checking without changing it would wrongly send you to a firmware update. Type `CONFIG SIGNALGROUP 3 6` and wait for the module to reboot (bench, 25164: about 9 s, then it answers about 2 s later; a command sent meanwhile gets `The Board is resetting,do not response any command!`), then reconnect `screen` if needed.
   Without sky: `ANT1GROUPA` (no rate) lists the signals the group tracks on the master antenna. On 25164 it shows E6 in `3 6` and B2a in `7 0` (bench).
   E6 check: type `GPGSV 1`. Watch `$GAGSV` lines. The **last field before `*`** is the NMEA 4.11 signal ID:

   | Signal ID | Galileo signal |
   |---|---|
   | 1 | E5a |
   | 2 | E5b |
   | **5** | **E6 (what we need)** |
   | 7 | E1 |

   - `$GAGSV ... ,5*hh` present → **E6 tracked. Skip step 2.**
   - Only 1/2/7 → no E6. Before doing step 2, try `CONFIG SIGNALGROUP 7 0`, wait for the reboot, then `GPGSV 1` and look for signal ID **5 (B2a)** in `$GBGSV` lines. BeiDou IDs: 1 B1I, 3 B1C, 5 B2a, 6 B2b, 8 B3I, B B2I (N4 manual Table 7-34). B2a present → BeiDou has 4 bands → **skip step 2**.
   - Neither works → do step 2.
   - Give it a few minutes; needs Galileo sats in view.
   - `VERSIONA`, `CONFIG SIGNALGROUP` and `ANT1GROUPA` work on this board's Build 25164 (bench); the `GPGSV` check needs sky and is still to do. Error replies read `PARSING FAILED ...`. If a command doesn't respond, check the *Unicore Reference Commands Manual for N4 High Precision Products V2*.
7. Also confirm the master SMA works: with the antenna on J1 (the `M` port) you should get many sats and a position fix.

### Step 2: Update UM982 firmware (only if no E6)
- **Not needed on the received board:** Build 25164 accepts `3 6` and `7 0` and reports E6 and B2a in them (bench). Updating the receiver from the station is lower priority now; a newer `.pkg` would come from UPrecise later. The rest of this step is for boards with older firmware.
- UPrecise is **Windows only**. Options: borrow a Windows PC (easiest), or run a Windows VM (VMware Fusion free / Parallels) with USB passthrough of the CH340N (not verified on Apple Silicon).
- Firmware: latest UM982 `.pkg` from Unicore download center (https://en.unicore.com/download-center/) or request from Unicore/seller. Known build on GitHub: YanTechLab `UM982_R4.10Build23575`. **Not confirmed which build first adds E6**: check release notes or just re-test after.
- Steps: plug the unlabelled top-edge USB-C (UM982) into Windows → UPrecise → connect COM @ 115200 → **Receiver Upgrade** → tick Receiver 1 → Soft Reset → select `.pkg` → Start. About 5–10 min. **Don't unplug or let the PC sleep.**
- Re-run step 1 to confirm E6.
- Re-run the `3 6` and `7 0` checks from step 1 after updating.

### Step 3: Configure UM982 as a base and output RTCM on COM1
Onocoy wants: RTCM3 **1005, 1033, 1077, 1087, 1097, 1117, 1127, 1137, 1230** at 1 Hz (1005 can be slower, e.g. every 10–30 s). MSM4/5/6/7 all accepted. All constellations/bands enabled.

Draft command list, typed over the UM982 USB-C connection (COM2) but **outputting on COM1** (to the ESP32). **Verify syntax against the N4 command manual before saving.**
```
UNLOG COM1
MODE BASE TIME 60
RTCM1005 COM1 10
RTCM1033 COM1 10
RTCM1077 COM1 1
RTCM1087 COM1 1
RTCM1097 COM1 1
RTCM1117 COM1 1
RTCM1127 COM1 1
RTCM1137 COM1 1
RTCM1230 COM1 10
SAVECONFIG
```
- `UNLOG COM1` first: as delivered, COM1 sends a position line and a status report at 1 Hz (bench), which would mix with the RTCM.
- `MODE BASE TIME 60` = survey-in for 60 s. Onocoy validates the position itself over 24–36 h, so a rough survey-in is fine.
- COM1 baud should stay **115200** (as delivered, bench; matches the ESP32 firmware default).
- Optional: also output the same RTCM on COM2 temporarily to see it arriving on the Mac.

### Step 4: ESP32 firmware (NTRIP server to onocoy)
The **stock** ESP32 firmware cannot act as an NTRIP *server*: it is a small Arduino SD-card test sketch with no Wi-Fi (bench). Back it up first if you want it (`esptool --chip esp32s3 -p <port> read-flash 0 0x1000000 stock-esp32.bin`), then flash:

**esp32-ntrip-DUO**: https://github.com/danusha2345/esp32-ntrip-DUO_danusha (fork for UM98x: https://github.com/tabunslonopotamov/esp32-ntrip-DUO_um980). Based on ESP32-XBee. Supports ESP32-S3, 2 simultaneous NTRIP servers, web UI.
1. Download the **esp32s3** release, use `esp32s3-ntrip-duo-merged.bin`.
2. Plug the USB-C labelled **`ESP32S3`** (bottom edge) into a USB-A port on the Mac (C-to-C gives nothing). Either plug orientation should flash: esptool works over the ESP32-S3's built-in USB (bench), and the CH343P side has the usual auto-reset pair (inferred). → flash at offset **0x0** with https://web.esphome.io/ (Chrome/Edge). Hold **`BOT`** while plugging in if it won't enter download mode.
3. Join Wi-Fi AP `ntrip-DUO_danusha` (open) → http://192.168.4.1
4. **Change the UART pins** (firmware default for S3 is TX GPIO43 / RX GPIO44, which is wrong for this board):
   - **RX = GPIO1** (from UM982 TXD1)
   - **TX = GPIO2** (to UM982 RXD1)
   - 115200, 8N1, no flow control
5. **Leave SD logging off.** The firmware's default S3 SD pins (MISO GPIO2…) clash with GPIO2 here; this board's SD is on GPIO4–7.
6. Set home Wi-Fi (station mode) and enable admin password.
7. NTRIP server 1 → onocoy:
   - Host `servers.onocoy.com`, port `2101`
   - This is an **NTRIP 1.0** style device (ESP32-XBee based), so per onocoy docs: **mountpoint = onocoy credential username**, **password = credential password**, leave username empty.
8. (Optional) NTRIP server 2 → RTK2go or RTKDirect.

Alternative firmware: mctainsh UM98RTKServer (https://github.com/mctainsh/Esp32/tree/main/UM98RTKServer) auto-configures UM98x and pushes to 3 casters, but it's built for T-Display-S3 pins (GPIO13/12), so it would need code changes for GPIO1/2.

### Step 5: Onocoy account + verification
- [ ] Onocoy account at https://console.onocoy.com/explorer → Reference Stations → NTRIP Credentials → Add new credential (one per station).
- [ ] Within about 1 min of streaming, the station appears under NTRIP Servers with live stats.
- [ ] Validation takes **24–36 h**.
- [ ] Explorer "Signals" tab: target 4 bands on one constellation. With `3 6` that's **Galileo L1/L2/L5/L6** (BeiDou shows L1/L2/L6). With `7 0` it's **BeiDou L1/L2/L5/L6** (Galileo shows L1/L2/L5).
- [ ] Check quality scale after a day or two.
- Streaks (Streak Appreciation: up to +50%, reached at 125 qualifying days; a missed day halves the streak) need, every UTC day, **availability ≥ 0.98** (under about 29 min offline) and **quality ≥ 0.80**. Quality ≥ 0.80 is only possible with **4 bands** on one constellation (band reward 0.95): with 3 bands the band reward is 0.80, so quality stays below 0.80.
- **Never stream two units from one antenna, or one credential from two units, at the same time.** Onocoy flags duplicate/correlated streams as fraud.

### Step 6: Install
- Antenna on roof, 360° clear sky above 10° elevation, **rigid mount** (moves < 1–2 mm; movement degrades rewards).
- Signal quality is mostly sky visibility and cycle slips: add a ground plane, check the cable and connectors, and keep away from walls and other reflective surfaces (multipath).
- With survey-in base mode, moving the antenna later only needs a power-off, the move, and a power-cycle; onocoy re-validates the location (24–36 h).
- Power: a plain 5 V USB-A charger with an A-to-C cable (bench: C-to-C gives no power).
- Board indoors or in a weatherproof box; keep the ESP32's PCB antenna area clear of metal for Wi-Fi.
- Stable internet; about 1 kB/s; outbound only, no ports to open.
- Watch for nearby stations: rewards drop if more than 3 stations are within 15–50 km.

## Open questions to answer when it arrives
- [x] Which SMA is master ("M")? J1, the lower SMA on the GH-connector edge.
- [x] UM982 firmware version out of the box? R4.10 Build 25164 (compiled 2026-05-21).
- [x] E6 tracked out of the box? No: the delivered group `4 5` has no E6 (at most 3 bands). `3 6` adds E6 and `7 0` adds B2a on 25164 (bench, no antenna); tracking with sky still to confirm.
- [x] Stock ESP32 firmware: can it NTRIP-server to onocoy? No: an Arduino SD-card test sketch with no Wi-Fi.
- [ ] Do the Unicore commands above work as written? `VERSIONA`, `CONFIG SIGNALGROUP` and `ANT1GROUPA` do (bench); the restart takes about 9 s, not 30 s. The base-mode and RTCM commands are still to test.
- [ ] Does one USB-C cable power both halves? (VBUS OR-ing inferred, unconfirmed.)
- [ ] Does `S3` reset the receiver? LED colours, and does D2 follow ESP32 activity?

## Log
_(fill in during setup)_

| Date | Step | Result |
|---|---|---|
| 2026-10-14 | 0 | Photos front and back; layout table corrected. |
| 2026-10-14 | Ports | C-to-C: nothing on either port; USB-A works. `ESP32S3` port orientation-dependent (built-in USB or CH343P). |
| 2026-10-14 | ESP32 | Stock firmware: Arduino SD-card test sketch, no Wi-Fi; Secure Boot/flash encryption off; flash backed up. |
| 2026-10-14 | 1 (no antenna) | Receiver R4.10 Build 25164, group `4 5`, rover. Groups `4 5`, `3 6`, `5 0`, `7 0` accepted; E6 in `3 6`, B2a in `7 0` (`ANT1GROUPA`). Left on `4 5`. Sky checks wait for the antenna. |

## Reference links
- Onocoy connect guide: https://docs.onocoy.com/documentation/3.-become-a-miner/3.-connect-your-station-to-onocoy
- Onocoy quality scale: https://docs.onocoy.com/documentation/mining-rewards-breakdown/quality-scale
- UM982 user manual R1.8: https://en.unicore.com/uploads/file/UM982_User%20Manual_EN_R1.8.pdf
- Unicore download center (UPrecise, firmware, N4 command manual): https://en.unicore.com/download-center/
- SparkFun Unicore firmware update guide: https://docs.sparkfun.com/Update_Firmware_Unicore_GNSS_Receiver/
- esp32-ntrip-DUO: https://github.com/danusha2345/esp32-ntrip-DUO_danusha
- ESPHome Web flasher: https://web.esphome.io/

## Options considered (for context)
- **Alibaba UM980 + ESP32-S3** (C$216 + C$10): better chip (guaranteed 4 bands) but no pinout/docs, two trading-company resellers, mostly AI-rendered images. Passed because the ESP32↔UM980 wiring is unknown.
- **GNSS.STORE ELT0233** (UM980, €299.99 + shipping/duty): plug-and-play, more expensive.
- **SparkFun RTK Postcard** (LG290P quad-band, C$314 at RobotShop.ca): plug-and-play onocoy support, more expensive.
- No multi-board or soldered builds.
