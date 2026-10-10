# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""RTCM 3 framing and the message decoders the bench tools need.

Frame: 0xD3, 6 reserved bits (0), 10-bit payload length N, N payload bytes,
CRC-24Q (polynomial 0x1864CFB, init 0, no reflection, no final XOR) over the
preamble, the two length bytes and the payload, sent MSB first.

Decoded here: the message number and station ID of any frame, MSM1-7
headers and per-cell C/N0 (MSM4-7), 1005/1006 (antenna reference point),
1033 (descriptor strings) and 1230 (GLONASS code-phase biases). Bit
positions follow the public RTCM 10403.x layouts.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

PREAMBLE = 0xD3
MAX_PAYLOAD = 1023
CRC24Q_POLY = 0x1864CFB


def _crc24q_table() -> list[int]:
    table = []
    for i in range(256):
        crc = i << 16
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000:
                crc ^= CRC24Q_POLY
        table.append(crc & 0xFFFFFF)
    return table


_CRC_TABLE = _crc24q_table()


def crc24q(data: bytes | bytearray | memoryview) -> int:
    """CRC-24Q of `data` (the RTCM 3 parity)."""
    crc = 0
    table = _CRC_TABLE
    for b in data:
        crc = ((crc << 8) & 0xFFFFFF) ^ table[((crc >> 16) ^ b) & 0xFF]
    return crc


def build_frame(payload: bytes) -> bytes:
    """Wrap a payload into a complete RTCM 3 frame (header + CRC)."""
    n = len(payload)
    if n > MAX_PAYLOAD:
        raise ValueError(f"payload too long: {n} > {MAX_PAYLOAD}")
    head = bytes([PREAMBLE, (n >> 8) & 0x03, n & 0xFF]) + bytes(payload)
    return head + crc24q(head).to_bytes(3, "big")


def frame_ok(frame: bytes) -> bool:
    """True when `frame` is one complete frame with a valid CRC."""
    if len(frame) < 6 or frame[0] != PREAMBLE or frame[1] >> 2:
        return False
    n = ((frame[1] & 0x03) << 8) | frame[2]
    if len(frame) != n + 6:
        return False
    return crc24q(frame[: n + 3]) == int.from_bytes(frame[n + 3 :], "big")


def payload_of(frame: bytes) -> bytes:
    n = ((frame[1] & 0x03) << 8) | frame[2]
    return bytes(frame[3 : 3 + n])


class Bits:
    """Big-endian bit reader over a payload (bit 0 = MSB of byte 0)."""

    __slots__ = ("nbits", "value")

    def __init__(self, data: bytes):
        self.nbits = len(data) * 8
        self.value = int.from_bytes(data, "big")

    def u(self, pos: int, width: int) -> int:
        if width == 0:
            return 0
        if pos < 0 or pos + width > self.nbits:
            raise ValueError(f"read past the payload: bits {pos}+{width} of {self.nbits}")
        return (self.value >> (self.nbits - pos - width)) & ((1 << width) - 1)

    def s(self, pos: int, width: int) -> int:
        x = self.u(pos, width)
        if x >> (width - 1):
            x -= 1 << width
        return x


class BitWriter:
    """Big-endian bit writer, the inverse of Bits (used to build test frames)."""

    def __init__(self) -> None:
        self.value = 0
        self.nbits = 0

    def u(self, value: int, width: int) -> BitWriter:
        if value < 0 or value >= (1 << width):
            raise ValueError(f"{value} does not fit in {width} unsigned bits")
        self.value = (self.value << width) | value
        self.nbits += width
        return self

    def s(self, value: int, width: int) -> BitWriter:
        lo, hi = -(1 << (width - 1)), (1 << (width - 1)) - 1
        if not lo <= value <= hi:
            raise ValueError(f"{value} does not fit in {width} signed bits")
        return self.u(value & ((1 << width) - 1), width)

    def to_bytes(self) -> bytes:
        pad = (-self.nbits) % 8
        return (self.value << pad).to_bytes((self.nbits + pad) // 8, "big")


# ---------------------------------------------------------------------------
# Constellations, MSM signal IDs and the band buckets

MSM_SYSTEMS = {
    107: "GPS",
    108: "GLONASS",
    109: "Galileo",
    110: "SBAS",
    111: "QZSS",
    112: "BeiDou",
    113: "NavIC",
}

# Constellations Onocoy rewards (QZSS and NavIC have weight 0, SBAS earns nothing).
REWARDED_SYSTEMS = ("GPS", "GLONASS", "Galileo", "BeiDou")

# MSM signal ID (1-based, DF395 bit order) -> RINEX observation code.
SIGNAL_CODES: dict[str, dict[int, str]] = {
    "GPS": {2: "1C", 3: "1P", 4: "1W", 8: "2C", 9: "2P", 10: "2W", 15: "2S", 16: "2L",
            17: "2X", 22: "5I", 23: "5Q", 24: "5X", 30: "1S", 31: "1L", 32: "1X"},
    "GLONASS": {2: "1C", 3: "1P", 8: "2C", 9: "2P"},
    "Galileo": {2: "1C", 3: "1A", 4: "1B", 5: "1X", 6: "1Z", 8: "6C", 9: "6A", 10: "6B",
                11: "6X", 12: "6Z", 14: "7I", 15: "7Q", 16: "7X", 18: "8I", 19: "8Q",
                20: "8X", 22: "5I", 23: "5Q", 24: "5X"},
    "SBAS": {2: "1C", 22: "5I", 23: "5Q", 24: "5X"},
    "QZSS": {2: "1C", 9: "6S", 10: "6L", 11: "6X", 12: "6E", 13: "6Z", 15: "2S", 16: "2L",
             17: "2X", 22: "5I", 23: "5Q", 24: "5X", 30: "1S", 31: "1L", 32: "1X"},
    "BeiDou": {2: "2I", 3: "2Q", 4: "2X", 8: "6I", 9: "6Q", 10: "6X", 14: "7I", 15: "7Q",
               16: "7X", 22: "5D", 23: "5P", 24: "5X", 25: "7D", 30: "1D", 31: "1P", 32: "1X"},
    "NavIC": {22: "5A"},
}

# Friendlier names for the signals the UM982 can track.
SIGNAL_NAMES: dict[str, dict[int, str]] = {
    "GPS": {2: "L1C/A", 3: "L1P", 4: "L1W", 8: "L2C/A", 9: "L2P", 10: "L2W", 15: "L2C(M)",
            16: "L2C(L)", 17: "L2C", 22: "L5I", 23: "L5Q", 24: "L5", 30: "L1C-D", 31: "L1C-P", 32: "L1C"},
    "GLONASS": {2: "G1", 3: "G1P", 8: "G2", 9: "G2P"},
    "Galileo": {2: "E1C", 3: "E1A", 4: "E1B", 5: "E1", 6: "E1Z", 8: "E6C", 9: "E6A", 10: "E6B",
                11: "E6", 12: "E6Z", 14: "E5bI", 15: "E5bQ", 16: "E5b", 18: "E5abI",
                19: "E5abQ", 20: "E5ab", 22: "E5aI", 23: "E5aQ", 24: "E5a"},
    "QZSS": {2: "L1C/A", 9: "L6D", 10: "L6P", 11: "L6", 12: "L6E", 13: "L6Z", 15: "L2C(M)",
             16: "L2C(L)", 17: "L2C", 22: "L5I", 23: "L5Q", 24: "L5", 30: "L1C-D", 31: "L1C-P", 32: "L1C"},
    "BeiDou": {2: "B1I", 3: "B1Q", 4: "B1", 8: "B3I", 9: "B3Q", 10: "B3", 14: "B2I", 15: "B2Q",
               16: "B2", 22: "B2a-D", 23: "B2a-P", 24: "B2a", 25: "B2b", 30: "B1C-D", 31: "B1C-P", 32: "B1C"},
    "SBAS": {2: "L1C/A", 22: "L5I", 23: "L5Q", 24: "L5"},
    "NavIC": {22: "L5"},
}

BANDS = ("L1", "L2", "L5", "L6", "E5ab")


def _ids(lo: int, hi: int) -> set[int]:
    return set(range(lo, hi + 1))


# Band buckets as Onocoy's explorer shows them: L1 = L1/E1/B1/B1C/G1,
# L2 = L2/G2/E5b/B2I/B2b, L5 = L5/E5a/B2a, L6 = E6/B3I/QZSS L6, and the
# Galileo E5 AltBOC signal as its own fifth band. Same mapping as the
# station firmware (components/gnss_framing/gnss_rtcm3.c).
BAND_OF_SIGNAL: dict[str, dict[str, set[int]]] = {
    "GPS": {"L1": _ids(2, 4) | _ids(30, 32), "L2": _ids(8, 10) | _ids(15, 17), "L5": _ids(22, 24)},
    "GLONASS": {"L1": _ids(2, 3), "L2": _ids(8, 9)},
    "Galileo": {"L1": _ids(2, 6), "L6": _ids(8, 12), "L2": _ids(14, 16), "E5ab": _ids(18, 20),
                "L5": _ids(22, 24)},
    "QZSS": {"L1": {2} | _ids(30, 32), "L6": _ids(9, 13), "L2": _ids(15, 17), "L5": _ids(22, 24)},
    "BeiDou": {"L1": _ids(2, 4) | _ids(30, 32), "L6": _ids(8, 10), "L2": _ids(14, 16) | {25},
               "L5": _ids(22, 24)},
    "SBAS": {"L1": {2}, "L5": _ids(22, 24)},
    "NavIC": {"L5": {22}},
}


def band_of(system: str, sig_id: int) -> str | None:
    for band, ids in BAND_OF_SIGNAL.get(system, {}).items():
        if sig_id in ids:
            return band
    return None


def signal_label(system: str, sig_id: int) -> str:
    name = SIGNAL_NAMES.get(system, {}).get(sig_id)
    code = SIGNAL_CODES.get(system, {}).get(sig_id)
    if name and code:
        return f"{name}({code})"
    return name or code or f"sig{sig_id}"


# ---------------------------------------------------------------------------
# Message classification


def msg_type(payload: bytes) -> int | None:
    if len(payload) < 2:
        return None
    return (payload[0] << 4) | (payload[1] >> 4)


def station_id(payload: bytes) -> int | None:
    if len(payload) < 3:
        return None
    return ((payload[1] & 0x0F) << 8) | payload[2]


def msm_system(mtype: int) -> str | None:
    """Constellation of an MSM message number, None if it is not an MSM.

    SBAS 1101-1107 is reported as MSM here; callers that feed casters treat
    those numbers as suspect, because Unicore reuses 1104/1105 for its own
    content (see is_suspect_sbas)."""
    if 1071 <= mtype <= 1137 and 1 <= mtype % 10 <= 7:
        return MSM_SYSTEMS.get(mtype // 10)
    return None


def is_suspect_sbas(mtype: int) -> bool:
    return 1101 <= mtype <= 1107


def msm_number(mtype: int) -> int:
    return mtype % 10


MESSAGE_NAMES = {
    1005: "station ARP",
    1006: "station ARP + height",
    1007: "antenna descriptor",
    1008: "antenna descriptor + serial",
    1013: "system parameters",
    1019: "GPS ephemeris",
    1020: "GLONASS ephemeris",
    1033: "receiver/antenna descriptors",
    1041: "NavIC ephemeris",
    1042: "BeiDou ephemeris",
    1044: "QZSS ephemeris",
    1045: "Galileo F/NAV ephemeris",
    1046: "Galileo I/NAV ephemeris",
    1230: "GLONASS code-phase biases",
    4074: "Unicore proprietary",
}


def describe_type(mtype: int | None) -> str:
    if mtype is None:
        return "?"
    sys_ = msm_system(mtype)
    if sys_ is not None:
        return f"{sys_} MSM{msm_number(mtype)}"
    return MESSAGE_NAMES.get(mtype, "")


# ---------------------------------------------------------------------------
# MSM

# Bits per satellite and per cell, and where C/N0 sits inside the cell block.
# MSM1-3 carry only the rough range modulo 1 ms (10 bits) per satellite; the
# whole milliseconds (8 bits) come with MSM4 and up.
_MSM_SAT_BITS = {1: 10, 2: 10, 3: 10, 4: 18, 5: 36, 6: 18, 7: 36}
_MSM_CELL_BITS = {1: 15, 2: 27, 3: 42, 4: 48, 5: 63, 6: 65, 7: 80}
# (bits per cell before the C/N0 field, C/N0 width, scale)
_MSM_CNR = {4: (42, 6, 1.0), 5: (42, 6, 1.0), 6: (55, 10, 1.0 / 16), 7: (55, 10, 1.0 / 16)}

MSM_HEADER_BITS = 169  # up to and including the signal mask


@dataclass
class MsmCell:
    sat: int  # satellite number within the constellation (1-64)
    sig: int  # MSM signal ID (1-32)
    cnr: float | None  # dB-Hz, None for MSM1-3 or "not available"


@dataclass
class Msm:
    mtype: int
    system: str
    msm: int
    station_id: int
    epoch: int  # 30-bit epoch time field as sent
    mmb: bool  # multiple message bit: more MSMs follow for this epoch
    iods: int
    sats: list[int]
    sigs: list[int]
    cells: list[MsmCell] = field(default_factory=list)

    def epoch_seconds(self) -> tuple[float, float]:
        """(seconds into the period, period length in seconds).

        GPS, Galileo, QZSS, NavIC, SBAS: time of week; BeiDou: BDT time of
        week; GLONASS: day of week * 86400 + time of day (Moscow time), or
        time of day alone when the day field is 7 (unknown)."""
        if self.system == "GLONASS":
            dow = self.epoch >> 27
            tod = (self.epoch & ((1 << 27) - 1)) / 1000.0
            if dow == 7:
                return tod, 86400.0
            return dow * 86400.0 + tod, 604800.0
        return self.epoch / 1000.0, 604800.0


def decode_msm(payload: bytes) -> Msm:
    """Decode an MSM1-7 payload. Raises ValueError on a malformed one."""
    bits = Bits(payload)
    mtype = bits.u(0, 12)
    system = msm_system(mtype)
    if system is None:
        raise ValueError(f"{mtype} is not an MSM message")
    n = msm_number(mtype)
    stn = bits.u(12, 12)
    epoch = bits.u(24, 30)
    mmb = bool(bits.u(54, 1))
    iods = bits.u(55, 3)
    sat_mask = bits.u(73, 64)
    sig_mask = bits.u(137, 32)
    sats = [i + 1 for i in range(64) if sat_mask & (1 << (63 - i))]
    sigs = [i + 1 for i in range(32) if sig_mask & (1 << (31 - i))]
    nsat, nsig = len(sats), len(sigs)
    if nsat * nsig > 64:
        raise ValueError(f"{nsat} satellites x {nsig} signals exceeds 64 cells")
    pos = MSM_HEADER_BITS
    cell_mask = bits.u(pos, nsat * nsig)
    pos += nsat * nsig
    present = []
    for k in range(nsat * nsig):
        if cell_mask & (1 << (nsat * nsig - 1 - k)):
            present.append((sats[k // nsig], sigs[k % nsig]))
    ncell = len(present)
    sat_block = pos
    cell_block = sat_block + nsat * _MSM_SAT_BITS[n]
    end = cell_block + ncell * _MSM_CELL_BITS[n]
    if end > bits.nbits:
        raise ValueError(f"MSM payload too short: needs {end} bits, has {bits.nbits}")
    cells = []
    cnr_spec = _MSM_CNR.get(n)
    for idx, (sat, sig) in enumerate(present):
        cnr = None
        if cnr_spec is not None:
            before, width, scale = cnr_spec
            raw = bits.u(cell_block + ncell * before + idx * width, width)
            cnr = raw * scale if raw else None
        cells.append(MsmCell(sat, sig, cnr))
    return Msm(mtype, system, n, stn, epoch, mmb, iods, sats, sigs, cells)


# ---------------------------------------------------------------------------
# 1005 / 1006

WGS84_A = 6378137.0
WGS84_F = 1.0 / 298.257223563
WGS84_E2 = WGS84_F * (2.0 - WGS84_F)


def ecef_to_llh(x: float, y: float, z: float) -> tuple[float, float, float]:
    """ECEF metres -> WGS84 latitude, longitude (degrees), ellipsoidal height (m)."""
    lon = math.atan2(y, x)
    p = math.hypot(x, y)
    if p < 1e-9:
        lat = math.copysign(math.pi / 2, z)
        h = abs(z) - WGS84_A * math.sqrt(1 - WGS84_E2)
        return math.degrees(lat), math.degrees(lon), h
    lat = math.atan2(z, p * (1 - WGS84_E2))
    h = 0.0
    for _ in range(10):
        sin_lat = math.sin(lat)
        n = WGS84_A / math.sqrt(1 - WGS84_E2 * sin_lat * sin_lat)
        h = p / math.cos(lat) - n
        new_lat = math.atan2(z, p * (1 - WGS84_E2 * n / (n + h)))
        if abs(new_lat - lat) < 1e-13:
            lat = new_lat
            break
        lat = new_lat
    sin_lat = math.sin(lat)
    n = WGS84_A / math.sqrt(1 - WGS84_E2 * sin_lat * sin_lat)
    h = p / math.cos(lat) - n
    return math.degrees(lat), math.degrees(lon), h


def llh_to_ecef(lat_deg: float, lon_deg: float, h: float) -> tuple[float, float, float]:
    lat, lon = math.radians(lat_deg), math.radians(lon_deg)
    n = WGS84_A / math.sqrt(1 - WGS84_E2 * math.sin(lat) ** 2)
    return (
        (n + h) * math.cos(lat) * math.cos(lon),
        (n + h) * math.cos(lat) * math.sin(lon),
        (n * (1 - WGS84_E2) + h) * math.sin(lat),
    )


@dataclass
class Arp:
    mtype: int
    station_id: int
    itrf_year: int
    gps: bool
    glonass: bool
    galileo: bool
    ref_station: bool
    x: float
    y: float
    z: float
    height: float | None  # 1006 antenna height above the marker, metres

    def llh(self) -> tuple[float, float, float]:
        return ecef_to_llh(self.x, self.y, self.z)


def decode_arp(payload: bytes) -> Arp:
    bits = Bits(payload)
    mtype = bits.u(0, 12)
    if mtype not in (1005, 1006):
        raise ValueError(f"{mtype} is not 1005/1006")
    height = bits.u(152, 16) * 1e-4 if mtype == 1006 else None
    return Arp(
        mtype=mtype,
        station_id=bits.u(12, 12),
        itrf_year=bits.u(24, 6),
        gps=bool(bits.u(30, 1)),
        glonass=bool(bits.u(31, 1)),
        galileo=bool(bits.u(32, 1)),
        ref_station=bool(bits.u(33, 1)),
        x=bits.s(34, 38) * 1e-4,
        y=bits.s(74, 38) * 1e-4,
        z=bits.s(114, 38) * 1e-4,
        height=height,
    )


# ---------------------------------------------------------------------------
# 1033


@dataclass
class Descriptor:
    station_id: int
    antenna: str
    setup_id: int
    antenna_serial: str
    receiver_type: str
    receiver_firmware: str
    receiver_serial: str


def decode_1033(payload: bytes) -> Descriptor:
    if msg_type(payload) != 1033:
        raise ValueError("not a 1033")
    pos = 3  # after the 12-bit type and 12-bit station ID

    def counted() -> str:
        nonlocal pos
        if pos >= len(payload):
            raise ValueError("1033 truncated")
        n = payload[pos]
        if pos + 1 + n > len(payload):
            raise ValueError("1033 string runs past the payload")
        s = payload[pos + 1 : pos + 1 + n].decode("latin-1")
        pos += 1 + n
        return s

    stn = station_id(payload) or 0
    antenna = counted()
    if pos >= len(payload):
        raise ValueError("1033 truncated")
    setup = payload[pos]
    pos += 1
    ant_serial = counted()
    rx_type = counted()
    rx_fw = counted()
    rx_serial = counted()
    return Descriptor(stn, antenna, setup, ant_serial, rx_type, rx_fw, rx_serial)


# ---------------------------------------------------------------------------
# 1230

GLO_BIAS_SIGNALS = ("L1 C/A", "L1 P", "L2 C/A", "L2 P")


@dataclass
class GloBiases:
    station_id: int
    aligned: bool  # DF421 code-phase bias indicator
    biases: dict[str, float]  # metres


def decode_1230(payload: bytes) -> GloBiases:
    bits = Bits(payload)
    if bits.u(0, 12) != 1230:
        raise ValueError("not a 1230")
    aligned = bool(bits.u(24, 1))
    mask = bits.u(28, 4)
    pos = 32
    biases = {}
    for i, name in enumerate(GLO_BIAS_SIGNALS):
        if mask & (1 << (3 - i)):
            biases[name] = bits.s(pos, 16) * 0.02
            pos += 16
    return GloBiases(bits.u(12, 12), aligned, biases)


# ---------------------------------------------------------------------------


def summarize_frame(frame: bytes) -> str:
    """One-line description of a CRC-valid frame for capture logs."""
    payload = payload_of(frame)
    mtype = msg_type(payload)
    head = f"RTCM {mtype} len={len(payload)}"
    try:
        if mtype is None:
            return head + " (empty)"
        if is_suspect_sbas(mtype):
            return head + " SBAS MSM number (Unicore reuses 1104/1105; not decoded)"
        if msm_system(mtype):
            m = decode_msm(payload)
            cnrs = [c.cnr for c in m.cells if c.cnr is not None]
            mean = f" cnr={sum(cnrs) / len(cnrs):.1f}" if cnrs else ""
            sigs = ",".join(signal_label(m.system, s) for s in m.sigs)
            return (f"{head} {m.system} MSM{m.msm} stn={m.station_id} t={m.epoch} mmb={int(m.mmb)} "
                    f"sats={len(m.sats)} cells={len(m.cells)} sigs=[{sigs}]{mean}")
        if mtype in (1005, 1006):
            a = decode_arp(payload)
            lat, lon, h = a.llh()
            return (f"{head} stn={a.station_id} ecef=({a.x:.4f},{a.y:.4f},{a.z:.4f}) "
                    f"llh=({lat:.9f},{lon:.9f},{h:.4f})")
        if mtype == 1033:
            d = decode_1033(payload)
            return (f"{head} stn={d.station_id} ant={d.antenna!r} setup={d.setup_id} "
                    f"rx={d.receiver_type!r} fw={d.receiver_firmware!r} serial={d.receiver_serial!r}")
        if mtype == 1230:
            g = decode_1230(payload)
            b = " ".join(f"{k}={v:+.2f}m" for k, v in g.biases.items())
            return f"{head} stn={g.station_id} aligned={int(g.aligned)} {b}".rstrip()
        name = MESSAGE_NAMES.get(mtype)
        return f"{head} stn={station_id(payload)}" + (f" ({name})" if name else "")
    except ValueError as e:
        return f"{head} DECODE-ERROR {e}"
