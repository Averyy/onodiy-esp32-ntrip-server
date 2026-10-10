# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Build RTCM 3 test frames from the public message layouts.

Every frame here is constructed field by field (message number, station ID,
MSM header and masks, satellite and cell blocks, CRC-24Q); none is a copy of
receiver output. The tests cross-check the layouts against pyrtcm, an
independent decoder.
"""

from __future__ import annotations

from benchlib.rtcm import BitWriter, build_frame

_SAT_BITS = {4: 18, 5: 36, 6: 18, 7: 36}


def msm_payload(mtype: int, station: int, epoch: int, mmb: bool, cells: dict[tuple[int, int], float],
                iods: int = 0) -> bytes:
    """MSM4-7 payload. `cells` maps (satellite, signal ID) -> C/N0 in dB-Hz."""
    n = mtype % 10
    if n not in _SAT_BITS:
        raise ValueError("only MSM4-7 are built here")
    sats = sorted({s for s, _ in cells})
    sigs = sorted({g for _, g in cells})
    w = BitWriter()
    w.u(mtype, 12).u(station, 12).u(epoch, 30).u(int(mmb), 1).u(iods, 3).u(0, 7).u(0, 2).u(0, 2).u(0, 1).u(0, 3)
    sat_mask = 0
    for s in sats:
        sat_mask |= 1 << (64 - s)
    sig_mask = 0
    for g in sigs:
        sig_mask |= 1 << (32 - g)
    w.u(sat_mask, 64).u(sig_mask, 32)
    present = []
    for s in sats:
        for g in sigs:
            on = (s, g) in cells
            w.u(int(on), 1)
            if on:
                present.append((s, g))
    nsat, ncell = len(sats), len(present)
    # Satellite block, field by field (all satellites per field).
    for i in range(nsat):
        w.u(70 + i, 8)  # rough range, whole milliseconds
    if n in (5, 7):
        for _ in range(nsat):
            w.u(0, 4)  # extended satellite info
    for i in range(nsat):
        w.u(100 + i, 10)  # rough range modulo 1 ms
    if n in (5, 7):
        for _ in range(nsat):
            w.s(0, 14)  # rough phase-range rate
    # Cell block, field by field (all cells per field).
    if n in (4, 5):
        for _ in range(ncell):
            w.s(0, 15)  # fine pseudorange
        for _ in range(ncell):
            w.s(0, 22)  # fine phase-range
        for _ in range(ncell):
            w.u(5, 4)  # lock time indicator
        for _ in range(ncell):
            w.u(0, 1)  # half-cycle ambiguity
        for cell in present:
            w.u(int(round(cells[cell])), 6)  # C/N0, 1 dB-Hz
        if n == 5:
            for _ in range(ncell):
                w.s(0, 15)  # fine phase-range rate
    else:
        for _ in range(ncell):
            w.s(0, 20)  # fine pseudorange, extended
        for _ in range(ncell):
            w.s(0, 24)  # fine phase-range, extended
        for _ in range(ncell):
            w.u(300, 10)  # lock time indicator, extended
        for _ in range(ncell):
            w.u(0, 1)  # half-cycle ambiguity
        for cell in present:
            w.u(int(round(cells[cell] * 16)), 10)  # C/N0, 1/16 dB-Hz
        if n == 7:
            for _ in range(ncell):
                w.s(0, 15)  # fine phase-range rate
    return w.to_bytes()


def msm_frame(*args, **kwargs) -> bytes:
    return build_frame(msm_payload(*args, **kwargs))


def arp_payload(station: int, x: float, y: float, z: float, mtype: int = 1005, height: float = 0.0,
                itrf: int = 0) -> bytes:
    w = BitWriter()
    w.u(mtype, 12).u(station, 12).u(itrf, 6).u(1, 1).u(1, 1).u(1, 1).u(0, 1)
    w.s(round(x * 1e4), 38).u(0, 1).u(0, 1).s(round(y * 1e4), 38).u(0, 2).s(round(z * 1e4), 38)
    if mtype == 1006:
        w.u(round(height * 1e4), 16)
    return w.to_bytes()


def descriptor_payload(station: int, antenna: str, setup: int, ant_serial: str, rx_type: str,
                       rx_fw: str, rx_serial: str) -> bytes:
    w = BitWriter()
    w.u(1033, 12).u(station, 12)
    for text, then_setup in ((antenna, True), (ant_serial, False), (rx_type, False), (rx_fw, False),
                             (rx_serial, False)):
        raw = text.encode("ascii")
        w.u(len(raw), 8)
        for b in raw:
            w.u(b, 8)
        if then_setup:
            w.u(setup, 8)
    return w.to_bytes()


def glo_bias_payload(station: int, aligned: bool, biases_m: dict[int, float]) -> bytes:
    """biases_m: index 0..3 (L1 C/A, L1 P, L2 C/A, L2 P) -> metres."""
    w = BitWriter()
    mask = 0
    for i in biases_m:
        mask |= 1 << (3 - i)
    w.u(1230, 12).u(station, 12).u(int(aligned), 1).u(0, 3).u(mask, 4)
    for i in sorted(biases_m):
        w.s(round(biases_m[i] / 0.02), 16)
    return w.to_bytes()


def nmea(body: str) -> str:
    """`$<body>*hh` with the NMEA checksum (XOR of the body, `$` excluded)."""
    x = 0
    for c in body.encode():
        x ^= c
    return f"${body}*{x:02X}"


def unicore_line(body: str) -> str:
    """`$<body>*hh` with the receiver-reply checksum (XOR including the `$`)."""
    x = ord("$")
    for c in body.encode():
        x ^= c
    return f"${body}*{x:02X}"
