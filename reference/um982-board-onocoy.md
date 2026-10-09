# UM982 + ESP32-S3 board → onocoy miner

Setup and test notes for the AliExpress UM982 + ESP32-S3 board, used as an onocoy reference station. Written 2026-10-06 before the board arrived. Pick up from **Setup plan** when it shows up.

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

## Board layout (from seller photos)

Board silkscreen: `RTK_EVB`.

| Part | Detail |
|---|---|
| GNSS module | Unicore **UM982**, label P/N 2310415000012, S/N A2254419032 in the photo (a PN covered by Unicore's UM982 manual R1.8). Front, left side next to the SMAs. |
| MCU | **ESP32-S3-WROOM-1 N16R8** (16 MB flash, 8 MB PSRAM), Wi-Fi + BLE 5 |
| 2× SMA (female, "inner hole") | ANT1 master + ANT2 slave. **Use the master only.** A small **"M"** is printed next to the lower SMA; that's probably the master. Confirm in step 1. |
| USB-C "UM982 UART2" | UM982 **COM2 → CH340N → USB**. For PC access: UPrecise, firmware upgrade, config, raw data. |
| USB-C "ESP32S3 USB&&UART" | ESP32-S3 via **CH343P**. For flashing/debugging the ESP32. BOOT + RST buttons next to it. |
| GH1.25-6P connector (white, front, below the UM982, labelled "UM982 UART3") | UM982 **COM3**. Pin order per back silkscreen: **5V, GND, TXD3, RXD3, EVENT, PPS**. Not needed. |
| TF (microSD) slot | **Back** of the board, on ESP32 SPI (GPIO4–7). Not needed. |
| 2× pin headers (unpopulated holes, P1/P2) | Spare ESP32-S3 GPIO, read from a low-res photo so treat as approximate. P1: GND, IO12, IO11, IO10, IO9, IO8, IO18, IO17, IO16, IO15, 3V3. P2: GND, GND, 5V, 5V, IO21, IO14, IO13, IO3, IO2, IO1, 3V3. **P2's IO1/IO2 are the UM982 COM1 lines.** |
| Buttons | ESP32: **BOOT** and **RST** (next to the ESP32 USB-C). Small button marked **S3** with **RST** nearby at the top-left, near the UM982 UART2 USB-C; probably the UM982 reset (unconfirmed). |
| USB chips | CH343P (ESP32 side, front), CH340N (UM982 COM2, back) |

### Seller's pin map

| ESP32-S3 | Connected to |
|---|---|
| **GPIO1** ← | **UM982 TXD1** (ESP RX) |
| **GPIO2** → | **UM982 RXD1** (ESP TX) |
| GPIO4 | SD MISO |
| GPIO5 | SD MOSI |
| GPIO6 | SD SCK |
| GPIO7 | SD CS |

| UM982 | Connected to |
|---|---|
| COM1 (TXD1/RXD1) | ESP32-S3 GPIO1/GPIO2 |
| COM2 (TXD2/RXD2) | CH340N → "UM982 UART2" USB-C |
| COM3 (TXD3/RXD3) | GH1.25-6P connector |

So the data path for onocoy is: **UM982 COM1 → ESP32 (GPIO1) → Wi-Fi → servers.onocoy.com:2101**.

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
- **Two ways to 4 bands (0.95)**, from the SIGNALGROUP tables in the *Unicore Reference Commands Manual for N4 High Precision Products V2* R1.4 (Tables 4-31 and 4-32). Changing it reboots the module and saves itself. Leaving out the second parameter sets the slave to 0.

  | `CONFIG SIGNALGROUP` | Master antenna signals | 4-band constellation | Slave antenna |
  |---|---|---|---|
  | `4 5` (default) | BDS B1I/B2I/B3I, GPS L1/L2/L5, GLO G1/G2, GAL E1/E5a/E5b | None, max 3 bands → 0.80 | On |
  | `3 6` | BDS B1I/B3I/B1C/B2b, GPS L1/L2/L5, GLO G1/G2, GAL E1/E5a/E5b/**E6** | **Galileo** | On |
  | `7 0` | BDS B1I/B2I/B3I/B1C/**B2a**/B2b, GPS L1/L2/L5, GLO G1/G2, GAL E1/E5a/E5b | **BeiDou** | Off ("for base station mode only") |
  | `5 0` | Low-power set, dual-frequency | None | Off |

- **`3 6` vs `7 0`:** both give 0.95.
  - `3 6` adds Galileo E6 but drops BeiDou B2I and B2a.
  - `7 0` keeps all 6 BeiDou signals, drops E6, and turns the unused slave antenna off.
  - Test both on the bench and keep whichever shows more signals on the onocoy explorer.
- **Firmware:** Unicore added BDS B1C/B2b, Galileo E6 and QZSS L6 to the UM982 spec in R1.8, so older firmware may lack E6. It's unconfirmed whether older builds support group `7 0`. If `7 0` works, a firmware update for E6 may not be needed at all.

### Current station baseline (2026-10-06)
- Live view: 35 sats, 121 signals, latency 0.63 s, mean C/N0 45.6 dBHz, code RMS 0.152 m, phase RMS 0.0019 m.
- Bands: GPS L1/L2/L5 (7/7/6), GLONASS L1/L2 (9/7), **Galileo L1/L2/L5/L6 (9/9/9/9)**, **BeiDou L1/L2/L5/L6 (19/10/10/10)** → already 4 bands → band reward 0.95.
- **Quality scale = 0.77** → no streak (needs ≥ 0.80).
- Math: constellation reward 1.0 (G+R+E+C) × band 0.95 × signal quality → signal quality ≈ **0.81**. Streak needs signal quality ≥ **0.842**.
- Code RMS (~0.98 score) and phase RMS (~0.96) look fine, so the shortfall is **sky visibility and/or cycle slips**. That's an antenna placement/obstruction/cable issue, **not the receiver**.
- An earlier sky view showed untracked (dark) sats on the W/SW/S side (E09, E12, E31, C40) and N (R18), which hints at an obstruction to the west/southwest.
- **Later roof/ridge move (separate from the hardware swap):** with survey-in base mode (`MODE BASE TIME 60`) just move the antenna (power off first) and power-cycle. No receiver reconfig. If a fixed base position was set instead, update it. Onocoy re-validates the location (24–36 h).
- **Install:** antenna is mounted on a **balcony on the side of the house**, so the house wall blocks part of the lower sky (likely the west side) and adds wall multipath. Roof/higher mount planned later but not possible now. Interim options: extend the antenna 0.5–1 m out from the wall on an arm, mount at the top of the railing, add a ground plane.
- **The new UM982 board won't raise quality** on the same antenna and mount. At best it matches the current station (with E6). Without E6 it drops to about 0.65 (0.80 × 0.81).
- To get streaks: improve sky view (raise antenna, clear W/SW horizon above 10°), add a ground plane, check cable/connectors for cycle slips, avoid nearby reflective surfaces.

## Safety

- Connect or disconnect the antenna (and cable) **with the board unpowered**, especially a fixed outdoor or roof antenna. Mains adapters can put a stray voltage on the chassis, and that can kill the receiver.
- Powering up with no antenna is fine. Leaving the slave SMA empty permanently is fine.
- Don't short an SMA centre pin to its shell (the port carries antenna bias voltage).
- Use a **plain 5V** USB supply. No fast-charge (QC/PD) supplies or no-name hubs.

## Setup plan

### Step 0: Unbox and check
- [ ] Board matches photos: UM982, ESP32-S3-WROOM-1, 2× SMA, 2× USB-C, TF slot.
- [ ] Note the UM982 label P/N and S/N.
- [ ] Find the "M" marking by the SMA connectors.

### Step 1: Check firmware version + E6, on the Mac (no Windows)
1. Antenna on the master SMA (power off first), antenna outside or with good sky view.
2. Plug **"UM982 UART2"** USB-C into the Mac.
3. `ls /dev/cu.*` → expect `/dev/cu.usbserial-XXXX` or `/dev/cu.wchusbserial-XXXX` (macOS has a built-in CH34x driver).
4. `screen /dev/cu.usbserial-XXXX 115200` (quit: Ctrl-A, K, Y).
5. Firmware version: type `VERSIONA` + Enter. If no reply, try `LOG VERSIONA ONCE`. Record the build string here: `__________`
6. **Set the signal group first.** The default `4 5` never tracks E6, so checking without changing it would wrongly send you to a firmware update. Type `CONFIG SIGNALGROUP 3 6` and wait for the module to reboot (about 30 s), then reconnect `screen` if needed.
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
   - These commands are from the Unicore N4 command set and haven't been tested on this board. If they don't respond, check the *Unicore Reference Commands Manual for N4 High Precision Products V2*.
7. Also confirm which SMA is master: with the antenna on the "M" port you should get many sats and a position fix.

### Step 2: Update UM982 firmware (only if no E6)
- UPrecise is **Windows only**. Options: borrow a Windows PC (easiest), or run a Windows VM (VMware Fusion free / Parallels) with USB passthrough of the CH340N (not verified on Apple Silicon).
- Firmware: latest UM982 `.pkg` from Unicore download center (https://en.unicore.com/download-center/) or request from Unicore/seller. Known build on GitHub: YanTechLab `UM982_R4.10Build23575`. **Not confirmed which build first adds E6**: check release notes or just re-test after.
- Steps: plug "UM982 UART2" into Windows → UPrecise → connect COM @ 115200 → **Receiver Upgrade** → tick Receiver 1 → Soft Reset → select `.pkg` → Start. About 5–10 min. **Don't unplug or let the PC sleep.**
- Re-run step 1 to confirm E6.
- Re-run the `3 6` and `7 0` checks from step 1 after updating.

### Step 3: Configure UM982 as a base and output RTCM on COM1
Onocoy wants: RTCM3 **1005, 1033, 1077, 1087, 1097, 1117, 1127, 1137, 1230** at 1 Hz (1005 can be slower, e.g. every 10–30 s). MSM4/5/6/7 all accepted. All constellations/bands enabled.

Draft command list, typed over the "UM982 UART2" connection (COM2) but **outputting on COM1** (to the ESP32). **Verify syntax against the N4 command manual before saving.**
```
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
- `MODE BASE TIME 60` = survey-in for 60 s. Onocoy validates the position itself over 24–36 h, so a rough survey-in is fine.
- COM1 baud should stay **115200** (matches the ESP32 firmware default).
- Optional: also output the same RTCM on COM2 temporarily to see it arriving on the Mac.

### Step 4: ESP32 firmware (NTRIP server to onocoy)
First check whether the **stock** ESP32 firmware can act as an NTRIP *server* (push RTCM to an external caster). The listing only mentions "CORS account" use, which sounds like rover/client. If it can't, flash:

**esp32-ntrip-DUO**: https://github.com/danusha2345/esp32-ntrip-DUO_danusha (fork for UM98x: https://github.com/tabunslonopotamov/esp32-ntrip-DUO_um980). Based on ESP32-XBee. Supports ESP32-S3, 2 simultaneous NTRIP servers, web UI.
1. Download the **esp32s3** release, use `esp32s3-ntrip-duo-merged.bin`.
2. Plug **"ESP32S3 USB&&UART"** USB-C into the Mac → flash at offset **0x0** with https://web.esphome.io/ (Chrome/Edge). Hold BOOT while plugging in if it won't enter download mode.
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

### Step 5b: Swapping it in for the current onocoy station (same antenna, same spot)
- **Reuse the existing station's NTRIP credentials** in the new unit. Onocoy credentials belong to the station, not the hardware, so the station keeps its history.
- Onocoy docs say moving a station only means passing data validation again. With the same antenna on the same mount, the antenna position doesn't change. Expect no re-validation, but watch the explorer for a day.
- The receiver doesn't need a precise "reference station" position; onocoy works out the location itself (FAQ). Survey-in is fine.
- **Never stream the old and new unit at the same time** (same credential, or two credentials on one antenna). Onocoy flags duplicate/correlated streams as fraud.
- **Streak protection** (Streak Appreciation: up to +50%, reached at 125 qualifying days; a missed day **halves** the streak). Each day needs:
  - **Availability ≥ 0.98** → keep swap downtime well under **~29 min** in that UTC day (2% of 24 h).
  - **Quality ≥ 0.80** → only possible with **4 bands** (band reward 0.95). With 3 bands max the band reward is 0.80, so quality = 0.80 × signal quality < 0.80, and the streak would fail **every day**. So **don't swap until 4 bands on one constellation are confirmed** (E6 with `3 6`, or B2a with `7 0`).
- **Order:** do steps 1–4 on the bench first (E6 confirmed, firmware done, RTCM on COM1, ESP32 flashed and on Wi-Fi). Then: power off old unit → move antenna cable (power off) → power on new unit → confirm it shows as connected in the explorer within a minute or two.
- First day after the swap: check Live view (sats/signals, Galileo L6 present) and the next day's Quality and Availability numbers.

### Step 6: Install
- Antenna on roof, 360° clear sky above 10° elevation, **rigid mount** (moves < 1–2 mm; movement degrades rewards).
- Board indoors or in a weatherproof box; keep the ESP32's PCB antenna area clear of metal for Wi-Fi.
- Stable internet; about 1 kB/s; outbound only, no ports to open.
- Watch for nearby stations: rewards drop if more than 3 stations are within 15–50 km.

## Open questions to answer when it arrives
- [ ] Which SMA is master ("M")?
- [ ] UM982 firmware version out of the box?
- [ ] E6 tracked out of the box?
- [ ] Stock ESP32 firmware: can it NTRIP-server to onocoy?
- [ ] Do the Unicore commands above work as written?

## Log
_(fill in during setup)_

| Date | Step | Result |
|---|---|---|
| | | |

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
