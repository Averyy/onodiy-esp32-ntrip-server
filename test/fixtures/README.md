# Real-world test fixtures

Receiver output captured by other people, with independent decodes of it
(the oracles), so our parsers are tested against real streams and not only
against frames we built ourselves. The host tests (`test/host`, Unity) and
the bench tool tests (`tools/bench`, pytest) read the same files.

Nothing here comes from our own board yet. Every file is from a public
source with a permissive license, listed below with the exact upstream
commit; the license texts are in [`LICENSES/`](LICENSES). About 300 KB of
data and 370 KB of oracles.

## Files

### RTCM 3 streams (`rtcm/`)

| File | Source | License | Receiver / encoder | What it exercises |
|---|---|---|---|---|
| `centipede_LAX_30ep.rtcm3` | Centipede-RTK caster `crtk.net`, mount LAX, live capture 2026-10-10 | ODC DbCL 1.0, attribution to Centipede-RTK ([notice](LICENSES/ODC-DbCL-1.0-Centipede-RTK.txt)) | a native UM980 (its own RTCM encoder) | 30 epochs of 1077/1087/1097/1107/1117/1127/1137 MSM7: QZSS 1117 and SBAS 1107 with **zero satellites** every epoch, NavIC 1137 closing each epoch, BeiDou epoch time 14 s behind GPS, C/N0 in 1/16 dB-Hz; one 1230 |
| `centipede_ROBU1_35ep.rtcm3` | same caster, mount ROBU1 | same | a native UM980 | 35 epochs of 1077/1087/1097/1127: every 1127 announces BeiDou **signal 14 with no cell** (a 10 x 6 cell mask with 48 cells), one epoch whose **closing MSM never comes** (1077 and 1087 only) and one with 1077 and 1127 only; ephemerides; 1230 |
| `centipede_BGDD_30ep.rtcm3` | same caster, mount BGDD | same | Septentrio mosaic-X5 | 30 epochs of MSM4 (1074/1084/1094/1124, C/N0 in whole dB-Hz) and a 1230 every epoch |
| `go-gnss_<type>_frame.bin` (16 files) | [go-gnss/rtcm](https://github.com/go-gnss/rtcm) @ 6948afc (v0.0.9), `rtcm3/data/` | Apache-2.0 ([text](LICENSES/Apache-2.0-go-gnss-rtcm.txt)) | Septentrio PolaRx5 (per its 1033) | one frame each of 1005, 1006, 1033, 1074, 1077, 1084, 1087, 1094, 1097, 1104 (an MSM4 with **zero satellites**), 1107, 1114, 1117, 1124, 1127, 1230 |
| `gpsd_rtcm3.2.log` | [gpsd](https://gitlab.com/gpsd/gpsd) @ 22ada91, `test/daemon/rtcm3.2.log` | BSD-2-Clause ([text](LICENSES/BSD-2-Clause-gpsd.txt)) | JAVAD TRE_G3TH DELTA (an IGS stream, 2016) | a `#` comment header, 13 frames of the non-standard **message 63** among MSM7, ephemerides and 1033; every epoch is **closed by SBAS 1107** |
| `gpsd_ublox-zed-f9t-rtcm3.log` | gpsd @ 22ada91, `test/daemon/` | BSD-2-Clause | u-blox ZED-F9T | RTCM inside a binary (UBX) stream: **3 false 0xD3 candidates** that fail their CRC, frames found again after them, a cut frame at the end |
| `pygnssutils_ntrip_chunked.bin` | [pygnssutils](https://github.com/semuconsulting/pygnssutils) @ dd8d3ca, `tests/ntrip_chunked.bin` | BSD-3-Clause ([text](LICENSES/BSD-3-Clause-pygnssutils.txt)) | an NTRIP 2.0 caster relaying a Trimble Alloy | a raw **HTTP chunked body** (100-byte chunks that cut frames anywhere; the capture ends inside a chunk-size line); BeiDou sent as **two 1127 per epoch** (same epoch time, the first with the multiple-message bit set) |
| `pygnssutils_nmea_rtcm3_excerpt.log` | pygnssutils @ dd8d3ca, `tests/pygpsdata-rtcm3.log`, bytes 44002 to 72066 (cut at a sentence start and a line end) | BSD-3-Clause | a u-blox F9 base | **NMEA mixed with RTCM** and binary messages; one binary message ends in `#` right in front of `$GNRMC,084220.00,...` |
| `pyrtcm_NTRIP-USCL00CHL0.log` | [pyrtcm](https://github.com/semuconsulting/pyrtcm) @ 5d66c7a, `tests/pygpsdata-NTRIP-USCL00CHL0.log` | BSD-3-Clause ([text](LICENSES/BSD-3-Clause-pyrtcm.txt)) | Septentrio PolaRx5 (an IGS stream) | one of nearly every message type: **MSM6 and MSM7 of the same epoch** for seven constellations, SBAS with satellites, empty QZSS and NavIC |
| `pyrtcm_RTCMMSM3.log` | pyrtcm @ 5d66c7a, `tests/pygpsdata-RTCMMSM3.log` | BSD-3-Clause | not given | **MSM3** (no C/N0) |
| `pyunigps_mixed.log` | [pyunigps](https://github.com/semuconsulting/pyunigps) @ 56c015f, `tests/pygpsdata_mixed.log` | BSD-3-Clause ([text](LICENSES/BSD-3-Clause-pyunigps.txt)) | a UM981 plus u-blox data | NMEA, **receiver binary logs** and RTCM in one stream |
| `rtklib_GMSD7_12ep.rtcm3` | [RTKLIB](https://github.com/tomojitakasu/RTKLIB) @ 71db0ff, `test/data/rcvraw/GMSD7_20121014.rtcm3`, the first 12 epochs after its first epoch boundary | BSD-2-Clause ([text](LICENSES/BSD-2-Clause-RTKLIB.txt)) | Trimble NetR9 (2012) | MSM7 1077/1087/1117/1127 with **QZSS satellites**, 1007/1008/1033 and ephemerides; this encoder puts **GPS time into its BeiDou MSMs** (no 14 s offset) |

The Centipede excerpts are cut from 60 to 90 s live captures of three
stations, whole frames only (no frame was edited, each keeps its CRC). Every
1005 and 1006 (the contributors' antenna positions) and every 1033 (their
receiver serial numbers) were left out. They are test inputs, not a
correction service, and must not be served as one (see the notice).

### Receiver NMEA (`nmea/`)

| File | Source | License | Receiver | What it exercises |
|---|---|---|---|---|
| `gpsd_unicore-um621_excerpt.log` | gpsd @ 22ada91, `test/daemon/unicore-um621.log`, its first 297 lines | BSD-2-Clause | Unicore UM621 (R6.0.0.0Build2810) | a real Unicore NMEA capture: 151 satellite-in-view sentences from GP (with **SBAS numbers 33-64 among the GPS ones**), GL, GA and GB talkers, NMEA 4.11 signal IDs (GPS 1 and 8, Galileo 1 and 7, BeiDou 1 and 5), empty C/N0 fields; the gpsd `#` header lines carry no checksum |

### Receiver replies and logs (`unicore/`)

| File | Source | License | What it exercises |
|---|---|---|---|
| `sparkfun_reply_samples.json` | eight reply lines quoted in the source comments of the [SparkFun Unicore GNSS Arduino Library](https://github.com/sparkfun/SparkFun_Unicore_GNSS_Arduino_Library) @ 5ae30de (file and line in each entry) | MIT ([text](LICENSES/MIT-SparkFun.txt)) | the restart banner `$devicename,COM3`, an OK and an error acknowledgement (`PARSING FAILD NO MATCHING FUNC  <CMD>`: the spelling in these samples, which UM982 build 25164 writes `FAILED`, and **two spaces**, as on 25164), three base-mode logs (rover, survey-in, fixed base), two configuration dump lines. All carry the **2-hex XOR that includes the leading `$` or `#`** |
| `pyunigps_um981.log` | pyunigps @ 56c015f, `tests/pygpsdata_um981.log` | BSD-3-Clause | 39 **binary logs from a real UM981**: sync `AA 44 B5`, 24-byte header with the message length at offset 6, then the body and a CRC-32 (reflected 0xEDB88320, initial 0, no final XOR, the one the `#` logs use) over the frame including the sync, stored little-endian |

Not here on purpose: the receiver manual's own example lines (its version
log among them). They are the vendor's copyrighted text, also where another
project copied them; tests build lines in the same layout instead.

## Oracles (`oracle/`)

Decoded by programs that share no code with ours:

- **RTCM** (`<file>.msm.jsonl`, `index.json`): frames found by preamble
  0xD3, six zero reserved bits, the 10-bit length and CRC-24Q (bytes in
  between are skipped and counted), then every CRC-valid frame decoded by
  pyrtcm 1.2.0. For the upstream files the MSM content was cross-checked
  against go-gnss/rtcm v0.0.9 (type, satellites, signals, and per cell the
  C/N0, lock time and half-cycle flag): 0 mismatches. The Centipede excerpts
  consist of frames of those cross-checked captures, checked frame by frame
  against them.
- `<file>.msm.jsonl`: one line per MSM, in stream order:
  `off` (offset of its 0xD3 in the stream, after de-chunking for the chunked
  body), `type`, `sys`, `msm`, `station`, `epoch` (for GLONASS the time of
  day only; `dow` holds the day of week), `mmb` (multiple-message bit),
  `iods`, `epoch_idx` (the decoder's grouping by the multiple-message bit
  alone), `sats` and `sigs` (IDs from the masks, MSB = 1), `cells`
  (`[sat, sig, cnr]` in sent order; `cnr` is DF403 in dB-Hz for MSM4/5,
  DF408 in 1/16 dB-Hz for MSM6/7, `null` for MSM1-3).
- `index.json`, per fixture: SHA-256 and size; for RTCM files the frame
  count, CRC failures, skipped bytes, the cut frame at the end, frames per
  message number, the MSM count, the 1033 strings (`d1033`) and the number
  of checksum-valid text lines (`text_lines`: a line from `$` or `#` to CR or
  LF that verifies under the NMEA XOR, the lead-inclusive XOR or the CRC-32;
  for the NMEA sentences the same set pynmeagps finds).
- `gpsd_unicore-um621_excerpt.log.gsv.jsonl`: every satellite-in-view
  sentence decoded by pynmeagps with its checksum verified: talker, sentence
  count and number, satellites in view, the satellite blocks
  `[number, elevation, azimuth, C/N0]` (`null` = empty field) and the signal
  ID.

Where our parsers and the oracle differ by design:

- 1101 to 1107 (SBAS numbers): the oracle decodes them as MSMs; the station
  firmware frames and counts them but never decodes or forwards them (the
  receiver may reuse these numbers). The bench tools decode them.
- The oracle's `epoch_idx` ends an epoch only at a multiple-message bit of
  0. Our epoch rules also end it at a new epoch time (an epoch whose closing
  MSM never came); the tests compute that grouping from the oracle records.
- pyrtcm names IGS SSR messages (4076) by subtype; we count message number
  4076 (not among these fixtures).

## Regenerating

The oracles were produced on 2026-10-10 with pyrtcm 1.2.0 and pynmeagps
1.1.7 (Python 3.14) from exactly these files. To check a file against its
upstream copy, compare the SHA-256 in `index.json`. A new fixture needs its
license notice here and in `LICENSES/`, an entry in `index.json` and, for
RTCM, an `.msm.jsonl` in the format above.
