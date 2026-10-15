# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Talking to a Unicore UM982 over a serial port (its COM2 USB bridge).

Command syntax and reply formats are the public Unicore N4 command set:
plain ASCII commands ending CR LF; each command is acknowledged with
`$command,<echo>,response: OK*hh` (or `PARSING FAILED <reason>`, which
builds before 25164 spell `FAILD`), XOR including the `$`; queries then send
their data: `#` logs with a CRC-32 (`#VERSIONA`, `#OBSVMA`, ...) or a 2-hex
XOR (`#MODE`), `$CONFIG,...` lines, NMEA sentences, and `<` continuation
lines (`UNILOGLIST`). A receiver that restarts prints `$devicename,COMn` on
each port; a command sent while it restarts gets `The Board is
resetting,do not response any command!*39` (no `$`, the XOR of the text
before `*`; build 25164).

This module decides what may be sent (read-only allow-list, a block list,
and everything else = needs --write), runs one command at a time, and
parses the replies the bench tools summarise.
"""

from __future__ import annotations

import queue
import re
import threading
import time
from collections import defaultdict
from dataclasses import dataclass, field

from .common import iso
from .rtcm import best_systems, summarize_frame
from .stream import Item, StreamSplitter, crc32_unicore, xor_checksum

# ---------------------------------------------------------------------------
# What may be sent

# Commands that only report: the receiver's state does not change. A log
# name alone (no port, no rate) is output once on the port that asked.
READ_ONLY = {
    "VERSIONA": "identity: model, firmware build, authorisation, PN-serial, board ID, compile date",
    "CONFIG": "configuration dump: signal group, port bauds, RTCM options, base antenna, ...",
    "MODE": "operating mode (rover/base, survey-in or fixed)",
    "MASK": "elevation, constellation and C/N0 masks",
    "UNILOGLIST": "logs currently enabled on every port",
    "BESTNAVA": "one position solution (fix type, lat/lon/height, satellites)",
    "BESTNAVXYZA": "one position solution in ECEF",
    "BESTSATA": "satellites used in the solution (once)",
    "GPGSV": "satellites in view with C/N0 and NMEA signal IDs, all constellations (once)",
    "GPGGA": "one GGA fix sentence",
    "GPGSA": "one GSA sentence",
    "GPRMC": "one RMC sentence",
    "GPGST": "one GST sentence",
    "OBSVMA": "one epoch of master-antenna observations, one entry per tracked signal (once)",
    "AGCA": "automatic gain control levels (once)",
    "RTKSTATUSA": "RTK status (once)",
    "SATSINFOA": "satellite information (once)",
    "SATELLITEA": "visible satellites (once)",
    "BASEPOSA": "base station position (once)",
    "BASEINFOA": "base station information (once)",
    "RECTIMEA": "receiver time (once)",
    "JAMSTATUSA": "jamming detection (once)",
    "RTCMSTATUSA": "RTCM data status (once)",
}

# The day-one query set: run by `um982.py query` (the default), in this order.
DAY_ONE = ["VERSIONA", "CONFIG", "MODE", "MASK", "UNILOGLIST", "BESTNAVA", "GPGSV", "OBSVMA"]

_BLOCKED = [
    (re.compile(r"^\s*FRESET\b", re.I), "FRESET wipes the receiver's saved configuration, almanac and baud; never sent"),
    (re.compile(r"^\s*RESET\s+\S", re.I), "RESET with parameters clears stored receiver data; never sent (plain RESET is allowed with --write)"),
    (re.compile(r"T@"), "bootloader stop key; firmware/bootloader traffic is never sent by these tools"),
    (re.compile(r"^\s*\d+\s*$"), "a bare number is a bootloader menu key; never sent"),
    (re.compile(r"^\s*(UPGRADE|UPDATE|BOOTLOADER|BOOT|XMODEM|YMODEM|ERASE|LOADER)\b", re.I),
     "firmware/bootloader command; never sent"),
    (re.compile(r"[^\x20-\x7e]"), "control or non-ASCII characters; never sent"),
]

_WARNINGS = [
    (re.compile(r"^\s*CONFIG\s+SIGNALGROUP\b", re.I),
     "the receiver restarts by itself (about 30 s) when the group changes, and saves the group on its own"),
    (re.compile(r"^\s*SAVECONFIG\b", re.I), "writes the current configuration (logs, ports) to the receiver's flash"),
    (re.compile(r"^\s*CONFIG\s+COM2\b", re.I), "changes the port you are connected through; reopen at the new baud afterwards"),
    (re.compile(r"^\s*CONFIG\s+COM1\b", re.I), "changes the ESP32 link; the station firmware must follow the new baud"),
    (re.compile(r"^\s*UNLOG\s+COM1\b", re.I), "stops output to the ESP32: the station stops streaming until RTCM is enabled again"),
    (re.compile(r"^\s*RESET\s*$", re.I), "restarts the receiver; a survey-in starts over"),
    (re.compile(r"^\s*MODE\s+\S", re.I), "changes base/rover mode; a survey-in starts over"),
]


def normalise(cmd: str) -> str:
    return " ".join(cmd.strip().split())


def blocked_reason(cmd: str) -> str | None:
    for pat, why in _BLOCKED:
        if pat.search(cmd):
            return why
    if not cmd.strip():
        return "empty command"
    return None


def is_read_only(cmd: str) -> bool:
    return normalise(cmd).upper() in READ_ONLY


def warnings_for(cmd: str) -> list[str]:
    return [why for pat, why in _WARNINGS if pat.search(cmd)]


# ---------------------------------------------------------------------------
# Session

ACK = re.compile(r"^\$command,(.*),response:\s*(.*?)\*[0-9A-Fa-f]{2}$")


@dataclass
class Reply:
    command: str
    sent_at: float
    ack: Item | None = None
    ok: bool | None = None
    reason: str = ""
    data: list[Item] = field(default_factory=list)
    others: list[Item] = field(default_factory=list)
    elapsed: float = 0.0
    sends: int = 1

    @property
    def lines(self) -> list[str]:
        out = [self.ack.text] if self.ack else []
        return out + [d.text for d in self.data]

    @property
    def answered(self) -> bool:
        return self.ack is not None or bool(self.data)


def data_matcher(cmd: str):
    """Predicate for the data lines a command produces (None: ack only)."""
    c = normalise(cmd).upper()
    if c == "CONFIG" or c == "MASK":
        return lambda t: t.startswith("$CONFIG,")
    if c == "UNILOGLIST":
        return lambda t: t.startswith("#UNILOGLIST") or t.startswith("<")
    if c == "MODE":
        return lambda t: t.startswith("#MODE")
    if re.fullmatch(r"GP[A-Z]{3}", c):
        sentence = c[2:]
        return lambda t: re.match(rf"^\$G[A-Z]{sentence},", t) is not None
    if re.fullmatch(r"[A-Z0-9]+A", c):
        return lambda t: t.upper().startswith("#" + c + ",")
    return None


class UnicoreSession:
    """One port, one command at a time; every byte in and out is logged."""

    def __init__(self, ser, run_dir, echo=print):
        self.ser = ser
        self.echo = echo
        self.split = StreamSplitter()
        self.items: queue.Queue[Item] = queue.Queue()
        self.stop = threading.Event()
        self.rx_raw = open(run_dir / "session-rx.bin", "wb")
        self.log = open(run_dir / "session.log", "a", encoding="utf-8")
        self.banners: list[Item] = []
        self.frames: list[Item] = []  # CRC-valid RTCM frames seen on the port, in order
        self.lock = threading.Lock()
        self.reader = threading.Thread(target=self._read, name="um982-rx", daemon=True)
        self.reader.start()

    def _read(self) -> None:
        while not self.stop.is_set():
            try:
                waiting = self.ser.in_waiting
                data = self.ser.read(waiting) if waiting else self.ser.read(1)
            except Exception as e:  # port gone
                self._log(f"{iso()} RX-ERROR {e}")
                self.stop.wait(0.5)
                continue
            if not data:
                continue
            t = time.time()
            self.rx_raw.write(data)
            for it in self.split.feed(data, t):
                self._log_item(it)
                if it.kind == "text" and it.text.startswith("$devicename"):
                    self.banners.append(it)
                elif it.kind == "rtcm":
                    self.frames.append(it)
                self.items.put(it)

    def _log(self, line: str) -> None:
        with self.lock:
            self.log.write(line + "\n")
            self.log.flush()

    def _log_item(self, it: Item) -> None:
        if it.kind == "text":
            note = f"({it.note})" if it.note else ""
            self._log(f"{iso(it.t)} RX[{it.check}]{note} {it.text}")
        elif it.kind == "rtcm":
            self._log(f"{iso(it.t)} RX {summarize_frame(it.data)}")
        else:
            self._log(f"{iso(it.t)} RX {it.kind.upper()} {len(it.data)} bytes {it.note}".rstrip())

    def drain(self) -> list[Item]:
        out = []
        while True:
            try:
                out.append(self.items.get_nowait())
            except queue.Empty:
                return out

    def listen(self, seconds: float) -> list[Item]:
        end = time.time() + seconds
        out = []
        while time.time() < end:
            try:
                out.append(self.items.get(timeout=max(0.01, end - time.time())))
            except queue.Empty:
                break
        return out

    def write_raw(self, text: str) -> None:
        data = text.encode("ascii")
        self._log(f"{iso()} TX {text!r}")
        self.ser.write(data)
        self.ser.flush()

    def command(self, cmd: str, ack_timeout: float = 3.0, quiet: float = 0.7,
                max_data: float = 6.0, retries: int = 0) -> Reply:
        """Send one command and collect its ack and data lines.

        retries: extra sends when nothing at all came back within ack_timeout
        (use only for read-only queries)."""
        cmd = normalise(cmd)
        if blocked_reason(cmd):
            raise ValueError(f"refusing to send {cmd!r}: {blocked_reason(cmd)}")
        match = data_matcher(cmd)
        self.drain()  # lines that arrived before the command belong to nobody
        reply = Reply(cmd, time.time())
        for attempt in range(retries + 1):
            reply.sends = attempt + 1
            self.write_raw(cmd + "\r\n")
            reply.sent_at = time.time()
            ack_deadline = reply.sent_at + ack_timeout
            data_deadline = None
            last_data = None
            while True:
                now = time.time()
                if reply.ack is None and not reply.data:
                    if now >= ack_deadline:
                        break  # nothing came back
                elif match is None:
                    break  # ack-only command, acknowledged
                elif last_data is not None and now - last_data >= quiet:
                    break  # data came and the port went quiet
                elif data_deadline is not None and now >= data_deadline:
                    break  # ack without data, or data that never stops
                try:
                    it = self.items.get(timeout=0.05)
                except queue.Empty:
                    continue
                if it.kind != "text":
                    reply.others.append(it)
                    continue
                m = ACK.match(it.text)
                if m and normalise(m.group(1)).upper() == cmd.upper():
                    reply.ack = it
                    reply.reason = m.group(2).strip()
                    reply.ok = reply.reason.upper().startswith("OK")
                    data_deadline = time.time() + max_data
                    continue
                if match and match(it.text):
                    reply.data.append(it)
                    last_data = time.time()
                    if data_deadline is None:
                        data_deadline = time.time() + max_data
                    continue
                reply.others.append(it)
            if reply.answered:
                break
        reply.elapsed = time.time() - reply.sent_at
        status = "no answer" if not reply.answered else (
            f"{reply.reason or 'no ack'}, {len(reply.data)} data lines")
        self._log(f"{iso()} DONE {cmd!r}: {status} in {reply.elapsed:.2f} s ({reply.sends} send(s))")
        return reply

    def close(self) -> None:
        self.stop.set()
        self.reader.join(timeout=1.0)
        for it in self.split.flush():
            self._log_item(it)
        self.rx_raw.close()
        self.log.close()


# ---------------------------------------------------------------------------
# Parsers


def log_body(line: str) -> str:
    """Text between the header's `;` and the trailing `*checksum`."""
    body = line.split(";", 1)[1] if ";" in line else line
    if "*" in body:
        body = body.rsplit("*", 1)[0]
    return body


def parse_versiona(line: str) -> dict:
    body = log_body(line)
    header = line.split(";", 1)[0]
    fields = [f.strip().strip('"') for f in body.split(",")]
    out = {"raw": line, "header_fields": len(header.split(","))}
    if len(fields) < 6:
        out["error"] = f"expected >= 6 body fields, got {len(fields)}"
        return out
    pn, _, serial_no = fields[3].partition("-")
    m = re.search(r"Build(\d+)", fields[1])
    out.update(model=fields[0], firmware=fields[1], build=int(m.group(1)) if m else 0,
               authorisation=fields[2], pn=pn, serial=serial_no, efuse_id=fields[4], compile_date=fields[5])
    return out


def parse_config(lines: list[str]) -> dict[str, list[str]]:
    items: dict[str, list[str]] = defaultdict(list)
    for line in lines:
        if not line.startswith("$CONFIG,"):
            continue
        text = line.rsplit("*", 1)[0]
        parts = text.split(",", 2)
        if len(parts) == 3:
            items[parts[1].upper()].append(parts[2])
    return dict(items)


def signal_group(config: dict[str, list[str]]) -> tuple[int, int | None] | None:
    for text in config.get("SIGNALGROUP", []):
        nums = re.findall(r"\d+", text)
        if nums:
            return int(nums[0]), (int(nums[1]) if len(nums) > 1 else None)
    return None


def parse_mode(line: str) -> str:
    return log_body(line).strip().rstrip(",")


def parse_bestnav(line: str) -> dict:
    f = log_body(line).split(",")
    if len(f) < 15:
        return {"raw": line}
    try:
        return {"sol_status": f[0], "pos_type": f[1], "lat_deg": float(f[2]), "lon_deg": float(f[3]),
                "height_msl_m": float(f[4]), "undulation_m": float(f[5]),
                "sigma_m": [float(f[7]), float(f[8]), float(f[9])],
                "sats_tracked": int(f[13]), "sats_used": int(f[14])}
    except ValueError:
        return {"raw": line}


# NMEA 4.10/4.11 GSV signal IDs (Unicore's GNSS ID table) -> (name, Onocoy band)
NMEA_SYSTEMS = {"GP": "GPS", "GL": "GLONASS", "GA": "Galileo", "GB": "BeiDou", "BD": "BeiDou",
                "GQ": "QZSS", "GI": "NavIC"}
NMEA_SIGNALS = {
    "GPS": {"1": ("L1C/A", "L1"), "2": ("L1P", "L1"), "3": ("L1M", "L1"), "4": ("L2P", "L2"),
            "5": ("L2C-M", "L2"), "6": ("L2C-L", "L2"), "7": ("L5-I", "L5"), "8": ("L5-Q", "L5")},
    "GLONASS": {"1": ("G1C/A", "L1"), "2": ("G1P", "L1"), "3": ("G2C/A", "L2"), "4": ("G2P", "L2")},
    "Galileo": {"1": ("E5a", "L5"), "2": ("E5b", "L2"), "3": ("E5a+b", "E5ab"), "4": ("E6-A", "L6"),
                "5": ("E6-BC", "L6"), "6": ("E1-A", "L1"), "7": ("E1-BC", "L1")},
    "BeiDou": {"1": ("B1I", "L1"), "2": ("B1Q", "L1"), "3": ("B1C", "L1"), "4": ("B1A", "L1"),
               "5": ("B2a", "L5"), "6": ("B2b", "L2"), "7": ("B2a+b", "E5ab"), "8": ("B3I", "L6"),
               "9": ("B3Q", "L6"), "A": ("B3A", "L6"), "B": ("B2I", "L2"), "C": ("B2Q", "L2")},
    "QZSS": {"1": ("L1C/A", "L1"), "2": ("L1C(D)", "L1"), "3": ("L1C(P)", "L1"), "4": ("L1S", "L1"),
             "5": ("L2C-M", "L2"), "6": ("L2C-L", "L2"), "7": ("L5-I", "L5"), "8": ("L5-Q", "L5"),
             "9": ("L6D", "L6"), "A": ("L6E", "L6")},
}
# SBAS satellites come under the GPS talker with its signal IDs.
NMEA_SIGNALS["SBAS"] = NMEA_SIGNALS["GPS"]


@dataclass
class SignalView:
    """Signals per constellation: {system: {signal name: [(sat, cn0), ...]}}, plus bands."""

    source: str
    signals: dict = field(default_factory=lambda: defaultdict(lambda: defaultdict(list)))
    bands: dict = field(default_factory=lambda: defaultdict(set))

    def add(self, system: str, name: str, band: str | None, sat: str, cn0: float | None) -> None:
        self.signals[system][name].append((sat, cn0))
        if band and cn0:
            self.bands[system].add(band)

    def band_list(self, system: str) -> list[str]:
        order = ["L1", "L2", "L5", "L6", "E5ab"]
        return [b for b in order if b in self.bands.get(system, set())]

    def lines(self) -> list[str]:
        out = []
        for system in sorted(self.signals):
            parts = []
            for name, obs in self.signals[system].items():
                tracked = [c for _, c in obs if c]
                mean = f" {sum(tracked) / len(tracked):.0f}dB-Hz" if tracked else ""
                parts.append(f"{name} x{len(tracked)}{mean}")
            bands = "/".join(self.band_list(system)) or "-"
            out.append(f"{system:<8} bands {bands:<16} " + ", ".join(parts))
        return out

    def to_dict(self) -> dict:
        return {
            system: {
                "bands": self.band_list(system),
                "signals": {name: [{"sat": s, "cn0": c} for s, c in obs] for name, obs in sigs.items()},
            }
            for system, sigs in self.signals.items()
        }


def gsv_system(talker: str, number: int) -> str:
    """Constellation of one satellite-in-view entry (NMEA 4.11 numbering).

    The GPS and multi-constellation talkers also carry other systems by
    number range: a real Unicore capture reports SBAS satellites (33-64)
    under GP, among the GPS ones."""
    if talker in ("GP", "GN"):
        if 33 <= number <= 64 or 120 <= number <= 158:
            return "SBAS"
        if 65 <= number <= 96:
            return "GLONASS"
        if 193 <= number <= 202:
            return "QZSS"
        if 301 <= number <= 364:
            return "Galileo"
        if 401 <= number <= 463:
            return "BeiDou"
        return "GPS"
    return NMEA_SYSTEMS.get(talker, talker)


def parse_gsv(lines: list[str]) -> SignalView:
    view = SignalView("GSV")
    for line in lines:
        m = re.match(r"^\$(G[A-Z]|BD)GSV,", line)
        if not m:
            continue
        talker = m.group(1)
        fields = line.rsplit("*", 1)[0].split(",")
        rest = fields[4:]
        sig_id = None
        if len(rest) % 4 == 1:
            sig_id = rest[-1].strip().upper()
            rest = rest[:-1]
        for i in range(0, len(rest) - 3, 4):
            sat, cn0 = rest[i], rest[i + 3]
            if not sat:
                continue
            system = gsv_system(talker, int(sat)) if sat.isdigit() else NMEA_SYSTEMS.get(talker, talker)
            name, band = NMEA_SIGNALS.get(system, {}).get(sig_id, (f"sig{sig_id}" if sig_id else "?", None))
            view.add(system, name, band, sat, float(cn0) if cn0 else None)
    return view


# OBSVM channel tracking status: system in bits 16-18, signal type in bits 21-25.
OBS_SYSTEMS = {0: "GPS", 1: "GLONASS", 2: "SBAS", 3: "Galileo", 4: "BeiDou", 5: "QZSS", 6: "NavIC"}
OBS_SIGNALS = {
    "GPS": {0: ("L1C/A", "L1"), 3: ("L1C-P", "L1"), 11: ("L1C-D", "L1"), 9: ("L2P/L2C", "L2"),
            17: ("L2C(L)", "L2"), 6: ("L5-D", "L5"), 14: ("L5-P", "L5")},
    "GLONASS": {0: ("G1C/A", "L1"), 5: ("G2C/A", "L2"), 6: ("G3I", "G3"), 7: ("G3Q", "G3")},
    "SBAS": {0: ("L1C/A", "L1"), 6: ("L5I", "L5")},
    "Galileo": {1: ("E1B", "L1"), 2: ("E1C", "L1"), 12: ("E5a-P", "L5"), 17: ("E5b-P", "L2"),
                18: ("E6B", "L6"), 22: ("E6C", "L6")},
    "BeiDou": {0: ("B1I", "L1"), 4: ("B1Q", "L1"), 8: ("B1C-P", "L1"), 23: ("B1C-D", "L1"), 5: ("B2Q", "L2"),
               17: ("B2I", "L2"), 12: ("B2a-P", "L5"), 28: ("B2a-D", "L5"), 6: ("B3Q", "L6"), 21: ("B3I", "L6"),
               13: ("B2b", "L2")},
    "QZSS": {0: ("L1C/A", "L1"), 1: ("L1C/B", "L1"), 3: ("L1C-P", "L1"), 4: ("L1S", "L1"), 11: ("L1C-D", "L1"),
             6: ("L5-D", "L5"), 14: ("L5-P", "L5"), 17: ("L2C(L)", "L2"), 21: ("L6D", "L6"), 27: ("L6E", "L6")},
    "NavIC": {6: ("L5-D", "L5"), 14: ("L5-P", "L5")},
}


def parse_obsvm(line: str) -> SignalView:
    view = SignalView("OBSVM")
    f = log_body(line).split(",")
    if not f or not f[0].strip().isdigit():
        return view
    n = int(f[0])
    for k in range(n):
        g = f[1 + 11 * k : 1 + 11 * (k + 1)]
        if len(g) < 11:
            break
        try:
            prn = g[1]
            cn0 = int(g[7]) / 100.0
            status = int(g[10], 16)
        except ValueError:
            continue
        system = OBS_SYSTEMS.get((status >> 16) & 0x7, f"sys{(status >> 16) & 0x7}")
        sig = (status >> 21) & 0x1F
        name, band = OBS_SIGNALS.get(system, {}).get(sig, (f"type{sig}", None))
        view.add(system, name, band, prn, cn0 or None)
    return view


def four_band_systems(view: SignalView) -> list[str]:
    """The rewarded constellations (GPS, GLONASS, Galileo, BeiDou) that show all
    four reward bands (L1, L2, L5, L6) in this view, sorted.

    The same rule as um982.four_band and the station (rtcm.best_systems): a
    QZSS with four bands earns no band reward, so it is never listed."""
    return best_systems({s: view.band_list(s) for s in view.signals})


# ---------------------------------------------------------------------------
# Building receiver lines (simulators and tests): the formats the parsers
# above and the station firmware read, as in the public reply samples and the
# host-test vectors (test/host/main/um98x_test_vectors.h).


def xor_line(lead: str, body: str) -> str:
    """`$body*hh` or `#body*hh`, the XOR including the lead character."""
    return f"{lead}{body}*{xor_checksum(lead + body):02X}"


def ack_line(cmd: str, reply: str = "OK") -> str:
    """The acknowledgement of a command (`reply` OK, or the refusal text)."""
    return xor_line("$", f"command,{cmd},response: {reply}")


def refusal_text(cmd: str) -> str:
    """The receiver's refusal of a command it does not take (build 25164's words and double space)."""
    words = cmd.split()
    return "PARSING FAILED NO MATCHING FUNC  " + (words[-1] if words else "").upper()


# Build 25164's refusal of a signal group it does not offer (a trailing comma,
# no echo of the value; nothing changes and the receiver does not restart).
SG_REFUSAL = "PARSING FAILED GRAMMAR ERROR,"

# Its answer to any command sent while it restarts.
NOTICE_TEXT = "The Board is resetting,do not response any command!"

# What ANT1GROUPA / ANT2GROUPA (one-shot, no rate) answered on build 25164 in
# each signal group it accepts (main, second antenna input; bodies only, as
# recorded on our bench; test/fixtures/unicore/um982_build25164_replies.json).
GROUP_BODIES = {
    (4, 5): ("4,5,GP,0x1D,GL,0x03,GA,0x07,GB,0x07,GQ,0x15", "5,5,GP,0x0D,GL,0x03,GA,0x05,GB,0x07,GQ,0x05"),
    (3, 6): ("3,5,GP,0x1D,GL,0x03,GA,0x0F,GB,0x2D,GQ,0x35", "6,4,GP,0x0D,GA,0x05,GB,0x05,GQ,0x05"),
    (7, 0): ("7,5,GP,0x1D,GL,0x03,GA,0x07,GB,0x3F,GQ,0x15", "0,5,GP,0x1D,GL,0x03,GA,0x07,GB,0x3F,GQ,0x15"),
    (5, 0): ("5,5,GP,0x0D,GL,0x03,GA,0x05,GB,0x07,GQ,0x05", "0,5,GP,0x1D,GL,0x03,GA,0x07,GB,0x3F,GQ,0x15"),
}


def notice_line() -> str:
    """The restarting answer: no lead character, the XOR of the text before `*`."""
    return f"{NOTICE_TEXT}*{xor_checksum(NOTICE_TEXT):02X}"


def groups_line(body: str, ant: int = 1) -> str:
    """A signal-group log with `body` (CRC-32 over the bytes after the `#`).
    The header is made up: the bench recorded only the bodies."""
    text = f"ANT{ant}GROUPA,COM1,0,0.0,FINE,0,0.000,0,0,0;{body}"
    return f"#{text}*{crc32_unicore(text.encode('latin-1')):08x}"


def config_line(item: str, text: str) -> str:
    """One item of the configuration dump: `$CONFIG,<item>,<text>*hh`."""
    return xor_line("$", f"CONFIG,{item},{text}")


def banner_line(port: str = "COM1") -> str:
    """What a restarting receiver prints on each port."""
    return xor_line("$", f"devicename,{port}")


def mode_line(body: str) -> str:
    """The base-mode log (2-hex XOR including the `#`)."""
    return xor_line("#", f"MODEA,COM1,0,0.0,FINE,0,0.000,0,0,0;{body}")


def version_line(model: str, firmware: str, authorisation: str = "HRPT00-S10C-P",
                 pn_sn: str = "1234567890123-SYNTH00000001", efuse_id: str = "0123456789abcdef",
                 compile_date: str = "2025/04/18") -> str:
    """The version log (CRC-32 over the bytes after the `#`, lowercase hex)."""
    body = (f'VERSIONA,79,GPS,FINE,2350,349712000,0,0,18,975;"{model}","{firmware}","{authorisation}",'
            f'"{pn_sn}","{efuse_id}","{compile_date}"')
    return f"#{body}*{crc32_unicore(body.encode('latin-1')):08x}"
