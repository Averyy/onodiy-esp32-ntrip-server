# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Summarise an RTCM 3 stream: counts, rates, MSM content, station messages,
and the checks that matter for Onocoy."""

from __future__ import annotations

from collections import Counter, defaultdict
from dataclasses import dataclass, field

from . import rtcm
from .stream import Item

GPS_WEEK_S = 604800.0
GLO_TO_GPS_S = -3 * 3600 + 18  # Moscow time -> GPS time (18 leap seconds since 2017)
BDS_TO_GPS_S = 14


def _wrap(delta: float, period: float) -> float:
    return delta % period


class IntervalStats:
    """Running statistics of the gaps between successive events."""

    def __init__(self) -> None:
        self.last: float | None = None
        self.n = 0
        self.total = 0.0
        self.min: float | None = None
        self.max: float | None = None
        self.hist: Counter = Counter()  # gaps rounded to 10 ms

    def add(self, t: float, period: float | None = None) -> None:
        if self.last is not None:
            d = t - self.last
            if period:
                d = _wrap(d, period)
            if d > 0:
                self.n += 1
                self.total += d
                self.min = d if self.min is None else min(self.min, d)
                self.max = d if self.max is None else max(self.max, d)
                self.hist[round(d, 2)] += 1
        self.last = t

    @property
    def median(self) -> float | None:
        if not self.n:
            return None
        half = self.n / 2
        acc = 0
        for value in sorted(self.hist):
            acc += self.hist[value]
            if acc >= half:
                return value
        return None

    def to_dict(self) -> dict:
        return {
            "intervals": self.n,
            "median_s": self.median,
            "mean_s": self.total / self.n if self.n else None,
            "min_s": self.min,
            "max_s": self.max,
        }


@dataclass
class TypeStats:
    mtype: int
    frames: int = 0
    bytes: int = 0
    epochs: int = 0  # MSM: distinct epochs
    last_epoch: int | None = None
    intervals: IntervalStats = field(default_factory=IntervalStats)
    time_source: str = ""


@dataclass
class BandStats:
    cnr_sum: float = 0.0
    cnr_n: int = 0
    sats_sum: int = 0  # satellites carrying this band, summed over epochs
    sats_max: int = 0
    epochs_present: int = 0
    signals: Counter = field(default_factory=Counter)


@dataclass
class SystemStats:
    system: str
    epochs: int = 0
    sats_sum: int = 0
    sats_max: int = 0
    sats_seen: set = field(default_factory=set)
    bands: dict = field(default_factory=lambda: defaultdict(BandStats))
    types: Counter = field(default_factory=Counter)


class RtcmStats:
    def __init__(self) -> None:
        self.types: dict[int, TypeStats] = {}
        self.systems: dict[str, SystemStats] = {}
        self.crc_bad = 0
        self.crc_bad_types: Counter = Counter()
        self.text_lines = 0
        self.text_kinds: Counter = Counter()
        self.text_bad = 0
        self.junk_bytes = 0
        self.binary = 0
        self.decode_errors: Counter = Counter()
        self.arp_first: rtcm.Arp | None = None
        self.arp_last: rtcm.Arp | None = None
        self.arp_max_dev_m = 0.0
        self.arp_count = 0
        self.descriptors: Counter = Counter()
        self.last_descriptor: rtcm.Descriptor | None = None
        self.glo_biases: rtcm.GloBiases | None = None
        self.glo_bias_count = 0
        self.stream_time: float | None = None  # GPS seconds of week from the latest MSM
        self.host_first: float | None = None
        self.host_last: float | None = None
        # epoch accumulation per (system, epoch value)
        self._open: dict[str, tuple[int, dict]] = {}
        # MMB check: frames of one epoch burst
        self._burst_time: float | None = None
        self._burst_mmb: list[bool] = []
        self.bursts = 0
        self.bursts_mmb_ok = 0
        self.station_ids: Counter = Counter()

    # -- input ----------------------------------------------------------------

    def add_item(self, item: Item) -> None:
        if item.t is not None:
            if self.host_first is None:
                self.host_first = item.t
            self.host_last = item.t
        if item.kind == "rtcm":
            self.add_frame(item.data, item.t)
        elif item.kind == "rtcm_bad":
            self.crc_bad += 1
            self.crc_bad_types[item.rtcm_type] += 1
        elif item.kind == "text":
            self.text_lines += 1
            if item.check == "bad":
                self.text_bad += 1
            head = item.text.split(",", 1)[0][:16]
            self.text_kinds[head] += 1
        elif item.kind == "junk":
            self.junk_bytes += len(item.data)
        elif item.kind == "binary":
            self.binary += 1

    def add_frame(self, frame: bytes, host_t: float | None = None) -> None:
        payload = rtcm.payload_of(frame)
        mtype = rtcm.msg_type(payload)
        if mtype is None:
            return
        ts = self.types.get(mtype)
        if ts is None:
            ts = self.types[mtype] = TypeStats(mtype)
        ts.frames += 1
        ts.bytes += len(frame)
        sid = rtcm.station_id(payload)
        if sid is not None:
            self.station_ids[sid] += 1
        system = rtcm.msm_system(mtype)
        if system and not rtcm.is_suspect_sbas(mtype):
            try:
                m = rtcm.decode_msm(payload)
            except ValueError as e:
                self.decode_errors[f"{mtype}: {e}"] += 1
                return
            self._add_msm(ts, m)
            return
        # Non-MSM: time by the host clock if we have it, else by the stream.
        t, src = (host_t, "host") if host_t is not None else (self.stream_time, "stream")
        if t is not None:
            ts.intervals.add(t, GPS_WEEK_S if src == "stream" else None)
            ts.time_source = src
        try:
            if mtype in (1005, 1006):
                a = rtcm.decode_arp(payload)
                self.arp_count += 1
                if self.arp_first is None:
                    self.arp_first = a
                dev = ((a.x - self.arp_first.x) ** 2 + (a.y - self.arp_first.y) ** 2
                       + (a.z - self.arp_first.z) ** 2) ** 0.5
                self.arp_max_dev_m = max(self.arp_max_dev_m, dev)
                self.arp_last = a
            elif mtype == 1033:
                d = rtcm.decode_1033(payload)
                self.last_descriptor = d
                self.descriptors[(d.antenna, d.setup_id, d.antenna_serial, d.receiver_type,
                                  d.receiver_firmware, d.receiver_serial)] += 1
            elif mtype == 1230:
                self.glo_biases = rtcm.decode_1230(payload)
                self.glo_bias_count += 1
        except ValueError as e:
            self.decode_errors[f"{mtype}: {e}"] += 1

    def _add_msm(self, ts: TypeStats, m: rtcm.Msm) -> None:
        sec, period = m.epoch_seconds()
        if ts.last_epoch != m.epoch:
            ts.epochs += 1
            ts.intervals.add(sec, period)
            ts.time_source = "epoch"
            ts.last_epoch = m.epoch
        gps_t = self._gps_seconds(m, sec, period)
        if gps_t is not None:
            self.stream_time = gps_t
        self._mmb_track(gps_t, m.mmb)
        sysst = self.systems.get(m.system)
        if sysst is None:
            sysst = self.systems[m.system] = SystemStats(m.system)
        sysst.types[m.mtype] += 1
        key = m.system
        cur = self._open.get(key)
        if cur is None or cur[0] != m.epoch:
            if cur is not None:
                self._close_epoch(sysst, cur[1])
            cur = (m.epoch, {"sats": set(), "bands": defaultdict(set)})
            self._open[key] = cur
        acc = cur[1]
        acc["sats"].update(m.sats)
        sysst.sats_seen.update(m.sats)
        for c in m.cells:
            band = rtcm.band_of(m.system, c.sig) or f"sig{c.sig}"
            acc["bands"][band].add(c.sat)
            bs = sysst.bands[band]
            bs.signals[c.sig] += 1
            if c.cnr is not None:
                bs.cnr_sum += c.cnr
                bs.cnr_n += 1

    def _close_epoch(self, sysst: SystemStats, acc: dict) -> None:
        sysst.epochs += 1
        n = len(acc["sats"])
        sysst.sats_sum += n
        sysst.sats_max = max(sysst.sats_max, n)
        for band, sats in acc["bands"].items():
            bs = sysst.bands[band]
            bs.epochs_present += 1
            bs.sats_sum += len(sats)
            bs.sats_max = max(bs.sats_max, len(sats))

    @staticmethod
    def _gps_seconds(m: rtcm.Msm, sec: float, period: float) -> float | None:
        if m.system in ("GPS", "Galileo", "QZSS", "SBAS", "NavIC"):
            return sec % GPS_WEEK_S
        if m.system == "BeiDou":
            return (sec + BDS_TO_GPS_S) % GPS_WEEK_S
        if m.system == "GLONASS" and period == GPS_WEEK_S:
            return (sec + GLO_TO_GPS_S) % GPS_WEEK_S
        return None

    def _mmb_track(self, gps_t: float | None, mmb: bool) -> None:
        if gps_t is None:
            return
        if self._burst_time is not None and abs(gps_t - self._burst_time) > 0.0005:
            self._finish_burst()
        self._burst_time = gps_t
        self._burst_mmb.append(mmb)

    def _finish_burst(self) -> None:
        if self._burst_mmb:
            self.bursts += 1
            flags = self._burst_mmb
            if not flags[-1] and all(flags[:-1]):
                self.bursts_mmb_ok += 1
        self._burst_mmb = []

    def finish(self) -> None:
        for key, (_epoch, acc) in list(self._open.items()):
            self._close_epoch(self.systems[key], acc)
        self._open.clear()
        self._finish_burst()

    # -- results --------------------------------------------------------------

    def rate_hz(self, mtype: int) -> float | None:
        ts = self.types.get(mtype)
        if not ts or not ts.intervals.median:
            return None
        return 1.0 / ts.intervals.median

    def system_bands(self, system: str, min_fraction: float = 0.5) -> list[str]:
        """Bands seen in at least `min_fraction` of the constellation's epochs."""
        ss = self.systems.get(system)
        if not ss or not ss.epochs:
            return []
        return [b for b in rtcm.BANDS if b in ss.bands and ss.bands[b].epochs_present >= min_fraction * ss.epochs]

    def checks(self) -> list[dict]:
        """Onocoy-relevant checks: list of {check, status (PASS/WARN/FAIL/INFO), detail}."""
        out: list[dict] = []

        def add(check: str, status: str, detail: str) -> None:
            out.append({"check": check, "status": status, "detail": detail})

        for system in rtcm.REWARDED_SYSTEMS + ("QZSS",):
            msm_types = sorted(t for t in self.types if rtcm.msm_system(t) == system and not rtcm.is_suspect_sbas(t))
            optional = system == "QZSS"
            msm7 = [t for t in msm_types if rtcm.msm_number(t) == 7]
            if not msm_types:
                add(f"{system} MSM7 at 1 Hz", "INFO" if optional else "FAIL", "no MSM for this constellation")
                continue
            if msm7:
                t = msm7[0]
                hz = self.rate_hz(t)
                if hz is None:
                    add(f"{system} MSM7 at 1 Hz", "WARN", f"{t} seen once; no rate yet")
                elif 0.9 <= hz <= 1.1:
                    add(f"{system} MSM7 at 1 Hz", "PASS", f"{t} at {hz:.2f} Hz ({self.types[t].epochs} epochs)")
                elif hz > 1.1:
                    add(f"{system} MSM7 at 1 Hz", "FAIL", f"{t} at {hz:.2f} Hz: Onocoy ignores rates above 1 Hz")
                else:
                    add(f"{system} MSM7 at 1 Hz", "FAIL", f"{t} at {hz:.2f} Hz: lower rates earn less")
            else:
                lower = ", ".join(map(str, msm_types))
                status = "INFO" if optional else ("FAIL" if all(rtcm.msm_number(t) <= 3 for t in msm_types) else "WARN")
                add(f"{system} MSM7 at 1 Hz", status,
                    f"only {lower}: MSM4-6 are accepted, MSM1-3 are not rewarded; MSM7 is the target")
            levels = {rtcm.msm_number(t) for t in msm_types}
            if len(levels) > 1:
                add(f"{system} one MSM level", "WARN", f"several MSM levels at once ({', '.join(map(str, msm_types))})")

        if 1005 in self.types:
            hz = self.rate_hz(1005)
            per = f"every {1 / hz:.1f} s" if hz else "rate unknown"
            add("1005 present", "PASS", f"{self.types[1005].frames} frames, {per}")
        elif 1006 in self.types:
            add("1005 present", "WARN", "only 1006 (Onocoy lists 1005)")
        else:
            add("1005 present", "FAIL", "no 1005: Onocoy shows the station at 0,0 without it")

        for mtype, why in ((1033, "antenna/receiver descriptors"), (1230, "GLONASS code-phase biases")):
            if mtype in self.types:
                hz = self.rate_hz(mtype)
                per = f"every {1 / hz:.1f} s" if hz else "rate unknown"
                add(f"{mtype} present", "PASS", f"{self.types[mtype].frames} frames, {per}")
            else:
                add(f"{mtype} present", "WARN", f"missing ({why}; on Onocoy's required list)")

        # Only the rewarded constellations count (as on the station): a QZSS
        # with four bands earns no band reward.
        best = [f"{system} ({'/'.join(b for b in self.system_bands(system) if b in rtcm.REWARD_BANDS)})"
                for system in rtcm.best_systems({s: self.system_bands(s) for s in self.systems})]
        if best:
            add("constellation with 4 bands", "PASS", ", ".join(best))
        else:
            detail = "; ".join(f"{s}: {'/'.join(self.system_bands(s)) or '-'}" for s in sorted(self.systems))
            add("constellation with 4 bands", "FAIL", f"none ({detail or 'no MSM'}): band reward 0.80 instead of 0.95")

        gal = self.systems.get("Galileo")
        if gal is not None:
            e6 = gal.bands.get("L6")
            if e6 and e6.epochs_present:
                add("Galileo E6", "INFO", f"present in {e6.epochs_present}/{gal.epochs} epochs")
            else:
                add("Galileo E6", "INFO", "not seen (signal group without E6, or firmware without it)")

        odd = sorted(t for t in self.types if rtcm.is_suspect_sbas(t))
        if odd:
            add("SBAS/Unicore 110x types", "WARN", f"{', '.join(map(str, odd))} present; the station drops them")
        if 1137 in self.types:
            add("NavIC 1137", "INFO", "present (the UM982 tracks no NavIC; it carries nothing useful)")
        if self.crc_bad:
            add("RTCM CRC", "WARN", f"{self.crc_bad} frame candidates failed the CRC")
        if self.bursts:
            add("MMB per epoch", "PASS" if self.bursts_mmb_ok == self.bursts else "WARN",
                f"{self.bursts_mmb_ok}/{self.bursts} epoch bursts end with MMB=0 and have MMB=1 before")
        if self.arp_count > 1 and self.arp_max_dev_m > 0.001:
            add("1005 position stable", "INFO",
                f"moved up to {self.arp_max_dev_m * 1000:.1f} mm over {self.arp_count} frames (survey-in still running?)")
        return out

    def to_dict(self) -> dict:
        d: dict = {"messages": [], "systems": [], "station_ids": dict(self.station_ids)}
        for mtype in sorted(self.types):
            ts = self.types[mtype]
            d["messages"].append({
                "type": mtype, "name": rtcm.describe_type(mtype), "frames": ts.frames, "bytes": ts.bytes,
                "epochs": ts.epochs if rtcm.msm_system(mtype) else None,
                "rate_hz": self.rate_hz(mtype), "time_source": ts.time_source, **ts.intervals.to_dict(),
            })
        for system, ss in sorted(self.systems.items()):
            bands = {}
            for band, bs in sorted(ss.bands.items()):
                bands[band] = {
                    "epochs_present": bs.epochs_present,
                    "mean_sats": bs.sats_sum / ss.epochs if ss.epochs else None,
                    "max_sats": bs.sats_max,
                    "mean_cnr_dbhz": bs.cnr_sum / bs.cnr_n if bs.cnr_n else None,
                    "signals": {rtcm.signal_label(system, s): n for s, n in sorted(bs.signals.items())},
                }
            d["systems"].append({
                "system": system, "epochs": ss.epochs, "msm_types": dict(ss.types),
                "mean_sats": ss.sats_sum / ss.epochs if ss.epochs else None, "max_sats": ss.sats_max,
                "distinct_sats": sorted(ss.sats_seen), "bands": bands,
                "bands_present": self.system_bands(system),
            })
        if self.arp_last:
            a = self.arp_last
            lat, lon, h = a.llh()
            d["arp"] = {"type": a.mtype, "station_id": a.station_id, "itrf_year": a.itrf_year,
                        "ecef_m": [round(a.x, 4), round(a.y, 4), round(a.z, 4)],
                        "lat_deg": round(lat, 9), "lon_deg": round(lon, 9), "height_m": round(h, 4),
                        "antenna_height_m": a.height, "frames": self.arp_count,
                        "max_deviation_mm": round(self.arp_max_dev_m * 1000, 1),
                        "indicators": {"gps": a.gps, "glonass": a.glonass, "galileo": a.galileo,
                                       "ref_station": a.ref_station}}
        if self.last_descriptor:
            dd = self.last_descriptor
            d["descriptor_1033"] = {"station_id": dd.station_id, "antenna": dd.antenna, "setup_id": dd.setup_id,
                                    "antenna_serial": dd.antenna_serial, "receiver_type": dd.receiver_type,
                                    "receiver_firmware": dd.receiver_firmware,
                                    "receiver_serial": dd.receiver_serial, "variants": len(self.descriptors)}
        if self.glo_biases:
            g = self.glo_biases
            d["glonass_biases_1230"] = {"station_id": g.station_id, "aligned": g.aligned,
                                        "biases_m": g.biases, "frames": self.glo_bias_count}
        d["stream"] = {"crc_failures": self.crc_bad, "crc_failure_types": {str(k): v for k, v in self.crc_bad_types.items()},
                       "text_lines": self.text_lines, "text_bad_checksum": self.text_bad,
                       "text_heads": dict(self.text_kinds.most_common(20)), "junk_bytes": self.junk_bytes,
                       "binary_logs": self.binary, "decode_errors": dict(self.decode_errors),
                       "epoch_bursts": self.bursts, "epoch_bursts_mmb_ok": self.bursts_mmb_ok,
                       "host_seconds": (self.host_last - self.host_first) if self.host_first is not None else None}
        d["checks"] = self.checks()
        return d

    def report(self) -> str:
        d = self.to_dict()
        lines = []
        dur = d["stream"]["host_seconds"]
        lines.append("== Messages" + (f" (over {dur:.0f} s of host time)" if dur else ""))
        lines.append(f"{'type':>5}  {'name':<28} {'frames':>7} {'epochs':>7} {'bytes':>9} {'rate':>9}  interval min/median/max (s)  clock")
        for m in d["messages"]:
            rate = f"{m['rate_hz']:.3f} Hz" if m["rate_hz"] else "-"
            iv = "-"
            if m["median_s"] is not None:
                iv = f"{m['min_s']:.2f}/{m['median_s']:.2f}/{m['max_s']:.2f}"
            ep = m["epochs"] if m["epochs"] is not None else ""
            lines.append(f"{m['type']:>5}  {m['name'][:28]:<28} {m['frames']:>7} {ep:>7} {m['bytes']:>9} {rate:>9}  {iv:<28} {m['time_source']}")
        if d["systems"]:
            lines.append("")
            lines.append("== MSM content (per constellation and band; sats = mean per epoch / max)")
            for s in d["systems"]:
                lines.append(f"{s['system']}: {s['epochs']} epochs, sats {s['mean_sats']:.1f}/{s['max_sats']}, "
                             f"{len(s['distinct_sats'])} distinct; types {s['msm_types']}; "
                             f"bands {'/'.join(s['bands_present']) or '-'}")
                for band, b in s["bands"].items():
                    cnr = f"{b['mean_cnr_dbhz']:.1f} dB-Hz" if b["mean_cnr_dbhz"] is not None else "n/a"
                    sigs = ", ".join(f"{k} x{v}" for k, v in b["signals"].items())
                    lines.append(f"    {band:<5} sats {b['mean_sats']:.1f}/{b['max_sats']:<3} in {b['epochs_present']:>6} epochs  "
                                 f"C/N0 {cnr:<12} {sigs}")
        if "arp" in d:
            a = d["arp"]
            lines.append("")
            lines.append(f"== {a['type']} station {a['station_id']} (ITRF year field {a['itrf_year']})")
            lines.append(f"    ECEF  X {a['ecef_m'][0]:.4f}  Y {a['ecef_m'][1]:.4f}  Z {a['ecef_m'][2]:.4f} m")
            lines.append(f"    WGS84 lat {a['lat_deg']:.9f}  lon {a['lon_deg']:.9f}  h {a['height_m']:.4f} m (ellipsoidal)")
            if a["antenna_height_m"] is not None:
                lines.append(f"    antenna height {a['antenna_height_m']:.4f} m")
            lines.append(f"    {a['frames']} frames, moved up to {a['max_deviation_mm']} mm from the first")
        if "descriptor_1033" in d:
            x = d["descriptor_1033"]
            lines.append("")
            lines.append(f"== 1033 station {x['station_id']}")
            lines.append(f"    antenna {x['antenna']!r} setup {x['setup_id']} serial {x['antenna_serial']!r}")
            lines.append(f"    receiver {x['receiver_type']!r} firmware {x['receiver_firmware']!r} serial {x['receiver_serial']!r}")
            if x["variants"] > 1:
                lines.append(f"    ({x['variants']} different 1033 contents seen; this is the last)")
        if "glonass_biases_1230" in d:
            g = d["glonass_biases_1230"]
            b = ", ".join(f"{k} {v:+.2f} m" for k, v in g["biases_m"].items()) or "no biases"
            lines.append("")
            lines.append(f"== 1230 station {g['station_id']}: aligned={g['aligned']}, {b} ({g['frames']} frames)")
        st = d["stream"]
        lines.append("")
        lines.append(f"== Stream: {st['crc_failures']} CRC failures, {st['text_lines']} text lines "
                     f"({st['text_bad_checksum']} bad checksum), {st['junk_bytes']} junk bytes, "
                     f"{st['binary_logs']} binary logs")
        if st["text_heads"]:
            lines.append("    text: " + ", ".join(f"{k} x{v}" for k, v in st["text_heads"].items()))
        if st["decode_errors"]:
            lines.append("    decode errors: " + ", ".join(f"{k} x{v}" for k, v in st["decode_errors"].items()))
        lines.append("")
        lines.append("== Onocoy checks")
        for c in d["checks"]:
            lines.append(f"  [{c['status']:<4}] {c['check']:<28} {c['detail']}")
        return "\n".join(lines)

