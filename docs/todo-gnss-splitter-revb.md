# GNSS splitter rev B: update complete

Rev B replaces rev 1.0 in this repository. The old exports were removed rather than archived, as requested. See the [splitter README](../gnss-splitter/README.md) for the circuit, BOM, fabrication instructions, limits and files.

Back to [development](development.md).

Completed 2026-11-01:

- Copied the EasyEDA Pro release project, Gerbers, BOM, CPL, schematic PNG, PCB PDF, STEP model and IPC netlist from `~/code/easyeda-pcb/designs/gnss-splitter/`.
- Added top/bottom routing previews, computed RF output and SHA-256 checksums; checked copies against the source release.
- Replaced both repository splitter renders with the supplied rev B image, cropped and straightened.
- Updated the root README image description, historical delivered cost and folder description.
- Corrected the RF summary: 0.11–0.26 dB refers to individual input/output segments, not a complete antenna-to-receiver path. All RF figures remain labelled computed.
- Kept the hardware use case and station-policy reference; removed the original claim that two bias supplies double antenna current.

Boards were ordered 2026-11-01 and have not arrived. Physical RF measurements, antenna bias delivery and fit remain unverified; these are board validation tasks, not unfinished repository migration work.
