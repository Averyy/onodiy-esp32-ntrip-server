# GNSS Antenna Splitter

![GNSS Splitter 3D render](gnss-splitter-3d.png)

Budget active 1-to-2 GNSS antenna splitter PCB. Shares one antenna between two receivers while blocking DC bias voltage on one output so the receivers don't fight over antenna power. ~C$15 per board fully assembled from JLCPCB (5 board minimum), compared to commercial GNSS splitters at US$75+ that perform the same for residential use cases. If you don't want to order 5, reach out as I have extras on hand.

Designed in EasyEDA (rev 1.0, Jan 2025). Running full time since June 2025 splitting between a GEODNET miner and an Onocoy station with full rewards and signal on both. Pair with short [SMA patch cables](https://www.aliexpress.com/item/1005002380816726.html) to keep things tidy.

**Important:** This is a bare PCB with exposed SMA connectors. Wrap it in a non-conductive case or electrical tape to prevent shorts and reduce interference. Don't let it rest on metal surfaces.

## How it works

An active GNSS antenna has a built-in LNA that needs DC bias voltage (3-5V) from the receiver through the coax. When two receivers share one antenna, both try to supply bias, which can cause interference or damage.

This splitter solves it:
- **ANT** - SMA input from the antenna
- **DC-PASS** - output to the primary receiver (passes DC bias to power the antenna's LNA)
- **DC-BLOCK** - output to the secondary receiver (gets the RF signal but DC is blocked by a 47pF cap)

Only one receiver powers the antenna. The other gets a clean RF feed without any DC path.

## Schematic

```
                            ┌──────────────┐
                            │              ├──── DC-PASS (RF2)
  ANT (RF1) ──── C2 ───────┤   BP2G1+     │     SMA, bias voltage passes through
                            │   (U1)       │
                            │              ├──┬─ DC-BLOCK (RF3)
                            └──────────────┘  │  SMA, DC blocked
                                              │
                                           C1 47pF (DC block)
                                              │
                                           R1 200Ω (bias termination to GND)
```

## BOM

| Ref | Part | Package | Qty | LCSC # | Notes |
|-----|------|---------|-----|--------|-------|
| U1 | BP2G1+ (Mini-Circuits) | SMD 8-pin | 1 | C7459223 | Active GNSS splitter, 1-to-2 |
| RF1, RF2, RF3 | BWSMA-KWE-Z001 (BAT Wireless) | SMA female TH | 3 | C496551 | ANT, DC-PASS, DC-BLOCK |
| C1 | 47pF NP0 (Yageo CC0603JRNPO8BN470) | 0603 | 1 | C519392 | DC blocking cap on PORT_2 |
| C2 | 100nF (Samsung CL10B104KA8NNNC) | 0603 | 1 | C1590 | Input decoupling cap |
| R1 | 200Ω (Yageo RC0603FR-07200RL) | 0603 | 1 | C114664 | Bias termination on DC-blocked port |

## Fabrication

Upload the gerber ZIP to JLCPCB (or any PCB fab). All parts are available from LCSC for SMT assembly.

## Use case

Share one multi-band GNSS antenna between:
- A GEODNET miner (DC-PASS port, it supplies the antenna bias voltage)
- An Onocoy station (DC-BLOCK port, gets RF only, no DC conflict)

Or any two GNSS receivers that need to share an antenna without both trying to power it.

## Notes

- The DC-PASS port must always be connected to a receiver that supplies bias voltage to the antenna. An active GNSS antenna won't work on the DC-BLOCK port alone since that port has no DC path to power the antenna's LNA.
- The reason for DC-PASS and DC-BLOCK is that without isolation, two receivers would feed bias voltage into each other through the shared coax. This can overdrive the antenna's LNA with double the expected current and backfeed voltage into receiver ports that aren't designed for external DC input. The DC-BLOCK cap prevents this by letting only the RF signal through on the second output.

## Files

| File | Description |
|------|-------------|
| `SCH_GNSS-Splitter_2026-10-09.json` | EasyEDA schematic (editable source) |
| `PCB_PCB_GNSS-Splitter_2026-10-09.json` | EasyEDA PCB layout (editable source) |
| `Gerber_GNSS-Splitter_PCB_GNSS-Splitter_2026-10-09.zip` | Gerber files for PCB fabrication |
| `BOM_GNSS-Splitter_2026-10-09.csv` | Bill of materials with LCSC part numbers |
| `Schematic_GNSS-Splitter_2026-10-09.pdf` | Schematic PDF (viewable without EasyEDA) |
| `PCB_PCB_GNSS-Splitter_2026-10-09.pdf` | PCB layout PDF |
| `PCB_GNSS-Splitter_2026-10-09.dxf` | Board outline (DXF) |
