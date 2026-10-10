# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Serial ports: listing, identification, safe opening, listen-only probing.

Modem lines. Opening a tty raises DTR and RTS on most systems. On this board
the ESP32's CH343P bridge drives the usual auto-reset pair (EN and IO0) from
DTR/RTS: the chip resets only while exactly one of the two is asserted. The
tools therefore leave both asserted, as the OS sets them on open (pyserial's
default), and never toggle them, so attaching a capture does not reset the
ESP32. The only exception is capture.py --reset-esp32, an explicit request.
Whether the UM982 side's CH340N lines reach the receiver's reset is not
known; the tools never toggle them either.
"""

from __future__ import annotations

import re
import time
from dataclasses import dataclass, field

import serial
from serial.tools import list_ports

from .stream import StreamSplitter

# (VID, PID) -> (role, description) for this board's bridges.
KNOWN_USB = {
    (0x1A86, 0x55D3): ("esp32-console", "WCH CH343 (the board's ESP32-S3 console bridge)"),
    (0x1A86, 0x7523): ("um982-com2", "WCH CH340 family (the board's UM982 COM2 bridge is a CH340N)"),
    (0x1A86, 0x7522): ("um982-com2", "WCH CH340 variant"),
    (0x1A86, 0x5523): ("um982-com2", "WCH CH341/CH340 variant"),
    (0x303A, 0x1001): ("esp32-native-usb", "ESP32-S3 native USB (not wired on this board)"),
}

ROLE_TEXT = {
    "esp32-console": "ESP32 console (UART0, 115200)",
    "um982-com2": "UM982 COM2 (receiver USB port)",
    "esp32-native-usb": "ESP32 native USB",
}

PROBE_BAUDS = (115200, 460800, 921600, 230400, 57600, 38400, 19200, 9600)

_ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
_IDF_LOG = re.compile(r"(?m)^[IWEDV] \(\d+\) [\w.\-]+:")
_ROM = re.compile(r"ESP-ROM:|rst:0x[0-9a-f]+|boot:0x[0-9a-f]+|Guru Meditation|Backtrace:")


@dataclass
class PortInfo:
    device: str
    vid: int | None
    pid: int | None
    serial_number: str | None
    location: str | None
    description: str
    manufacturer: str | None
    role: str | None = None  # from VID/PID
    role_note: str = ""
    probe: ProbeResult | None = None

    @property
    def usb_id(self) -> str:
        if self.vid is None:
            return "-"
        return f"{self.vid:04x}:{self.pid:04x}"

    @property
    def final_role(self) -> str | None:
        """Content evidence wins over VID/PID."""
        if self.probe and self.probe.role:
            return self.probe.role
        return self.role


@dataclass
class ProbeResult:
    role: str | None = None
    baud: int | None = None
    evidence: str = ""
    tried: list[str] = field(default_factory=list)


def list_serial_ports(include_all: bool = False) -> list[PortInfo]:
    ports = []
    for p in sorted(list_ports.comports(), key=lambda p: p.device):
        if not include_all and p.vid is None and not p.device.startswith(("/dev/ttyUSB", "/dev/ttyACM")):
            # Bluetooth, debug consoles and other non-USB ttys.
            continue
        info = PortInfo(p.device, p.vid, p.pid, p.serial_number, p.location, p.description or "",
                        p.manufacturer)
        known = KNOWN_USB.get((p.vid, p.pid)) if p.vid is not None else None
        if known:
            info.role, info.role_note = known
        ports.append(info)
    return ports


def open_serial(device: str, baud: int, timeout: float = 0.1) -> serial.Serial:
    """Open a port for reading without touching the modem lines (see module doc)."""
    ser = serial.Serial()
    ser.port = device
    ser.baudrate = baud
    ser.timeout = timeout
    ser.write_timeout = 2.0
    ser.dtr = True  # the state the OS sets on open; no edge afterwards
    ser.rts = True
    try:
        ser.exclusive = True  # refuse to share the port with another tool
    except (AttributeError, ValueError):
        pass
    ser.open()
    return ser


def pulse_esp32_reset(ser: serial.Serial) -> None:
    """EN low for 100 ms through the auto-reset pair, IO0 left high (normal boot).

    Toggles modem lines only; sends no bytes."""
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False


def classify_bytes(data: bytes) -> tuple[str | None, str]:
    """Guess what a port carries from a sample. Returns (role, evidence)."""
    if not data:
        return None, "silent"
    text = _ANSI.sub("", data.decode("latin-1"))
    idf = len(_IDF_LOG.findall(text))
    rom = len(_ROM.findall(text))
    sp = StreamSplitter()
    items = sp.feed(data) + sp.flush()
    rtcm = sum(1 for it in items if it.kind == "rtcm")
    nmea = sum(1 for it in items if it.kind == "text" and it.check == "nmea")
    uni = sum(1 for it in items if it.kind == "text" and it.check in ("unicore", "crc32"))
    banner = any(it.kind == "text" and it.text.startswith("$devicename") for it in items)
    if idf or rom:
        return "esp32-console", f"{idf} ESP-IDF log lines, {rom} ROM/panic markers"
    if rtcm or nmea or uni:
        parts = []
        if rtcm:
            parts.append(f"{rtcm} RTCM3 frames (CRC ok)")
        if nmea:
            parts.append(f"{nmea} NMEA sentences")
        if uni:
            parts.append(f"{uni} receiver replies/logs" + (" incl. restart banner" if banner else ""))
        return "um982-com2", ", ".join(parts)
    return None, f"{len(data)} bytes, nothing valid (wrong baud?)"


def probe_port(device: str, listen_s: float = 1.5, bauds=PROBE_BAUDS, log=None) -> ProbeResult:
    """Listen (never transmit) at each baud until the content identifies the port."""
    res = ProbeResult()
    for baud in bauds:
        try:
            ser = open_serial(device, baud, timeout=0.05)
        except (serial.SerialException, OSError) as e:
            res.evidence = f"cannot open: {e}"
            res.tried.append(f"{baud}: cannot open")
            return res
        data = bytearray()
        try:
            end = time.monotonic() + listen_s
            while time.monotonic() < end:
                chunk = ser.read(4096)
                if chunk:
                    data += chunk
        finally:
            ser.close()
        role, evidence = classify_bytes(bytes(data))
        res.tried.append(f"{baud}: {evidence}")
        if log:
            log(f"  {device} @ {baud}: {evidence}")
        if role:
            res.role, res.baud, res.evidence = role, baud, evidence
            return res
        if not data:
            # Silence at one baud means silence at all of them: stop early.
            res.evidence = "silent (no bytes while listening)"
            return res
    res.evidence = "bytes at every baud tried, nothing valid"
    return res


def find_port(role: str, probe: bool = False, log=None) -> str | None:
    """Pick the port with `role` (by VID/PID, then by listening if asked)."""
    ports = list_serial_ports()
    hits = [p for p in ports if p.role == role]
    if len(hits) == 1:
        return hits[0].device
    if probe:
        for p in ports:
            if p.role in (None, role) or len(hits) > 1:
                p.probe = probe_port(p.device, log=log)
        hits = [p for p in ports if p.final_role == role]
        if len(hits) == 1:
            return hits[0].device
    return None
