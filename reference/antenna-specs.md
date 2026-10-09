# Antenna and cable

Antenna and cable options for UM982 onocoy stations and GEODNET miners. Prices checked 2026-10-09.

## What the antenna has to cover

- **GEODNET needs L1/L2/L5.** Every GEODNET station is a triple-frequency, full-constellation receiver, so the antenna must pass L5/E5a/B2a as well as L1 and L2.
- **onocoy needs 4 bands on one constellation** (band reward 0.95; 3 bands gives 0.80). The UM982 gets there two ways, set by `CONFIG SIGNALGROUP` (Unicore N4 command manual R1.4):
  - `3 6`: Galileo E1/E5a/E5b/**E6**, with the slave antenna on.
  - `7 0`: BeiDou B1/B2/**B2a**/B3, slave antenna off, "for base station mode only".
  - Either way the antenna has to pass E6 (1278.75 MHz) or B2a (1176.45 MHz) on top of the usual bands. Every suitable antenna below passes both.
- **Minimum band list for both:** GPS L1/L2/L5, Galileo E1/E5a/E5b/E6, BeiDou B1/B2/B3, GLONASS G1/G2.
- **L-band isn't needed.** It's the 1525-1559 MHz band for paid correction services. The UM982, UM980, onocoy and GEODNET don't use it.
- **One antenna per station.** onocoy flags two stations sharing one antenna, and GEODNET advises against splitters because they can cause interference.
- **Connectors:** the antennas below have a TNC female socket. The UM982 board and the GEODNET miner have SMA female sockets. The cable is TNC male to SMA male.

## The standard: HYFIX survey antenna

This is the reference antenna: the one GEODNET's store sells for its miners and ships in miner kits. Every other option below is measured against it.

- **Buy:** [GEODNET store](https://store.geodnet.com/products/multi-frequency-high-precision-survey-antenna) (SKU ACC-HPS01), US$95 with a 9 m cable included. Ships within a week. Orders outside the US ship DAP, so duties and brokerage are extra.
- **Bands:** GPS L1/L2/L5, GLONASS G1/G2, BeiDou B1/B2/B3, Galileo E1/E5a/E5b.
- **E6 isn't in its spec, but it passes E6 in practice.** On the owner's existing station (2026-10-09) it tracks Galileo E6 on 8 satellites and BeiDou B3 on 8. It also passes BeiDou B2a (7 satellites).
- **Gain per band (max):**
  - GPS: L1 ≥5.5 dBi, L2 ≥5.0, L5 ≥2.5
  - GLONASS: G1 ≥5.0, G2 ≥4.0
  - BeiDou: B1 ≥5.0, B2 ≥5.0, B3 ≥3.5
  - Galileo: E1 ≥5.5, E5a ≥2.5, E5b ≥5.0
- **Antenna element:** RHCP, axial ratio ≤3 dB, VSWR ≤1.5, phase centre error <±2 mm, multi-feed design.
- **LNA:** 38 ±2 dB gain, noise figure ≤1.8 dB, input/output VSWR ≤2.0, differential delay ≤5 ns, 3-5.5 V, ≤48 mA.
- **Build:** TNC female, Φ150 x 58.4 mm, ≤360 g, -45 to +70 °C operating.
- **Track record:** in a [2023 onocoy/GEODNET side-by-side test](https://simeononsecurity.com/other/unveiling-best-gps-antennas-onocoy-geodnet/) it performed as well as a US$135 Harxon CSX627A. Survey antennas costing about US$200 picked up 1-2 extra satellites but didn't earn more.
- **Source:** HYFIX "Multi-frequency High Precision Survey Antenna" datasheet, linked from the store page.

**Measured baseline** (the owner's existing station on the GEODNET miner, balcony mount with a partly blocked S/SW/W horizon, onocoy live view 2026-10-09). This is the bar any replacement antenna should meet.

| Satellites | Signals | Mean C/N0 | L1 (E1/B1) | L2 (E5b/B2) | L5 (E5a/B2a) | L6 (E6/B3) | Code RMS | Phase RMS |
|---|---|---|---|---|---|---|---|---|
| 32 | 105 | 45.5 dBHz | 46.6 dBHz | 45.7 dBHz | 44.8 dBHz | 44.7 dBHz | 0.242 m | 0.0013 m |

Bands per constellation: GPS L1/L2/L5, GLONASS L1/L2, Galileo L1/L2/L5/L6, BeiDou L1/L2/L5/L6.

## Pick: Beitian BT-800S

- **Bought:** 2 units: one for the UM982 onocoy station, one kept as a spare. The GEODNET miner keeps its HYFIX.
- **Buy:** [AliExpress listing 1005009033251316](https://www.aliexpress.com/item/1005009033251316.html), variant **"BT-800S"**. C$181.57 for two, all-in to Ontario. Also on [Beitian's store](https://store.beitian.com/products/beitian-rtk-mushroom-head-antenna-gnss-four-star-full-frequency-beidou-gps-high-precision-driving-test-bt-800s) at US$75.88 each.
- **Bands:** GPS L1/L2/L5, BeiDou B1I/B2I/B3I/B1C/B2a/B2b, GLONASS G1/G2/G3, Galileo E1/E5a/E5b/E6, QZSS L1/L2/L5/L6, SBAS L1/L5, NavIC L5, L-band.
- **Antenna element:** 5.5 dBi peak gain, axial ratio ≤3.0 dB, VSWR ≤2.0, phase centre error ±2 mm, RHCP.
- **LNA:** 38 ±2 dB gain, noise figure ≤2 dB, in-band flatness ±2 dB, 3.3-12 V, ≤45 mA.
- **Build:** TNC female, Φ154 x 64.2 mm, 420 g, IP67, -40 to +85 °C.
- **Source:** Beitian BT-800S datasheet rev 5.83 (2025-05).

**Against the HYFIX standard:**

| | HYFIX (standard) | BT-800S |
|---|---|---|
| GPS / GLONASS | L1/L2/L5, G1/G2 | L1/L2/L5, G1/G2/G3 |
| Galileo | E1/E5a/E5b (E6 too in practice) | E1/E5a/E5b/E6 |
| BeiDou | B1/B2/B3 | B1I/B2I/B3I/B1C/B2a/B2b |
| Gain | Per band, L5/E5a ≥2.5 dBi | 5.5 dBi peak only, no per-band figures |
| LNA gain / noise figure | 38 ±2 dB / ≤1.8 dB | 38 ±2 dB / ≤2 dB |
| Axial ratio / phase centre | ≤3 dB / <±2 mm | ≤3 dB / ±2 mm |
| Size / weight | Φ150 x 58.4 mm / ≤360 g | Φ154 x 64.2 mm / 420 g |
| Supply | 3-5.5 V, ≤48 mA | 3.3-12 V, ≤45 mA |
| Independent test | Yes (2023, matched a US$135 Harxon) | None found |
| Price | US$95 + shipping + duties, 9 m cable included | About C$91 each all-in, cable extra |

- **The BT-800S matches the standard on paper.** It lists E6, B1C/B2a/B2b and G3 where the standard doesn't. In practice the standard already passes E6 and B2a, so those extra bands add nothing measurable.
- **The standard is still the proven one.** Its noise figure is slightly lower, it publishes gain per band (L5 is usually the weak band on cheap antennas), and it has a real-world test behind it.

## How it compares

All options against the HYFIX standard.

| Option | Price | Link | vs the HYFIX standard |
|---|---|---|---|
| **HYFIX survey antenna (standard)** | US$95 + shipping + duties, 9 m cable included | [GEODNET store](https://store.geodnet.com/products/multi-frequency-high-precision-survey-antenna) | The reference. Tested and proven on GEODNET and onocoy. E6 isn't in its spec, but it tracks E6 in practice (8 Galileo satellites on the existing station). |
| **Beitian BT-800S** | C$181.57 for two (AliExpress, all-in). US$75.88 each (Beitian store) | [AliExpress](https://www.aliexpress.com/item/1005009033251316.html) "BT-800S" | **Pick.** Every band the standard has. Lists E6, B1C/B2a/B2b and G3 in its spec (the standard passes E6 and B2a in practice too). Same LNA gain. Noise figure ≤2 vs ≤1.8 dB. No per-band gain figures and no independent test. Full datasheet. "Industrial grade". |
| Beitian BT-760S | C$135.95 for two (AliExpress, all-in). US$45.88 each (Beitian store) | [AliExpress](https://www.aliexpress.com/item/1005009033251316.html) "BT-760S" | **Good alternative, best value.** Same listed specs as the BT-800S, so it also covers the standard's bands including E6, about C$23 less per antenna. Sold as "consumer grade", and no datasheet gives its element gain, axial ratio or phase centre. |
| Beitian BT-760 | US$35.88 (Beitian store) | [AliExpress](https://www.aliexpress.com/item/1005009033251316.html) "BT-760" | **Good budget alternative.** Covers the standard's bands including E6. No L-band (not needed). 5.0 dBi peak gain, 0.5 dB under the standard's L1/E1. 300 g. |
| Beitian BT-300S | US$70.88 (Beitian store) | [Beitian store](https://store.beitian.com/products/beitian-monitor-survey-cors-rtk-station-agriculture-autonomous-driving-gnss-gps-antenna-bt-300s) | Same bands as the BT-800S, so the standard's bands including E6. Larger: Φ160 x 66.5 mm, 400 g. |
| Beitian BT-800, BT-800D, BT-300, BT-300E, BT-47N | US$40.88-98.88 (Beitian store) | Same listings | **Not suitable.** Below the standard: GPS L1/L2 and Galileo E1/E5b only, with no L5, E5a or E6, so they fail GEODNET. The BT-800D did poorly on L5 in the 2023 test. |
| BDLXGNSS BD901 | US$44.51 | [AliExpress](https://www.aliexpress.com/item/1005009834053136.html) | **Not suitable.** Looks like the standard but isn't close. The seller's spec image lists GPS L1/L2, Galileo E1 and BeiDou B1/B2/B3 only. No L5, so it fails GEODNET. |

**Notes on the Beitian listing:**
- **Pick the variant by name.** Listing 1005009033251316 holds about 20 variants: antennas, mounts and cables. Its spec block shows BT-800D data, so ignore it. Each variant's price only shows once you select it.
- **The band lists are Beitian's claims.** No independent test of the BT-800S or BT-760S was found. After install, confirm E6 is tracked on the UM982: `$GAGSV` lines with signal ID 5 (see `um982-board-onocoy.md`, Step 1).

## Swapping an antenna on a live station

- **Power off first.** Connect or disconnect the antenna and cable with the receiver unpowered: the antenna port carries bias voltage.
- **Expect a position change.** A new antenna's phase centre sits a few cm from the old one (the housings are different heights).
  - onocoy revalidates the location, which takes 24-36 h.
  - GEODNET watches station position closely. Check the GEODNET console after the swap.
- **Check signal quality.** Compare mean and per-band C/N0 on the onocoy explorer against the HYFIX baseline above.

## Cable

- **Type:** RG58, TNC male to SMA male. It must be standard SMA (centre pin on the male), not RP-SMA.
- **Buy:** [AliExpress listing 1005005550779000](https://www.aliexpress.com/item/1005005550779000.html), lengths 0.3 m to 30 m, about US$1 to US$50 depending on length. Bought: one 6 m, one 8 m.
- **Alternative:** Beitian sells 3 m and 5 m "TNC-J - SMA-J" cables as variants on its antenna listings (US$8.88 / US$10.88 on its store). J means male.
- **GEODNET kit cable:** the 9 m cable that comes with GEODNET miners is TNC male to SMA male, so it fits the Beitian antennas directly.
- **Long runs:** GEODNET supports up to 18 m with two of its cables and an extender. Beyond that it suggests a TNC male to SMA male LMR-400 cable.

### Cable length and gain budget

- **Receiver window:** the UM982 and UM980 both want an "optimum input gain" of 18 dB min, 30 dB typical, 36 dB max at the antenna port (Unicore UM982 User Manual R1.8, UM980 User Manual R1.9).
- **Input gain = antenna LNA gain minus cable loss.** The HYFIX standard and the Beitian antennas all have a 38 ±2 dB LNA.
- **RG58 loss:** roughly 0.8-1 dB per metre at GNSS frequencies. This is a typical figure, not measured on this cable.

| RG58 length | Approx. loss | Input gain at the receiver |
|---|---|---|
| 1-2 m | 1-2 dB | 36-37 dB, at or over the max |
| 3-5 m | 3-5 dB | 33-35 dB |
| 6 m | 5-6 dB | 32-33 dB |
| 8 m | 6.5-8 dB | 30-31.5 dB |
| 10 m | 8-10 dB | 28-30 dB |
| 15 m | 12-15 dB | 23-26 dB |
| 20 m | 16-20 dB | 18-22 dB, at the floor |

- **Use 3-15 m of RG58.** Signal quality barely changes across that range, because the 38 dB LNA sets the system noise figure.
- **Past 15 m, use LMR-240 or LMR-400.** They lose much less per metre.
- **Under about 3 m,** the input can go over 36 dB.
