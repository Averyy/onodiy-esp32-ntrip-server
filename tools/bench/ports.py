#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""List serial ports and say which is the ESP32 console and which is the UM982.

The board has two USB-C ports:
  "ESP32S3 USB&&UART"  CH343P (USB 1a86:55d3)  -> ESP32-S3 UART0 console, 115200
  "UM982 UART2"        CH340N (USB 1a86:7523)  -> UM982 COM2, 115200 by default

Ports are identified by USB VID/PID first. Ports that VID/PID cannot place
(or every port, with --probe-all) are then probed by LISTENING at common
bauds and looking for ESP-IDF log lines, ROM boot text, NMEA, RTCM3 frames
or receiver replies. Nothing is ever written to any port, and the modem
lines are left as the OS sets them on open (no ESP32 reset).

Examples:
  uv run ports.py
  uv run ports.py --probe-all --listen 3
  uv run ports.py --json
"""

from __future__ import annotations

import argparse
import json
import sys

from benchlib.common import add_log_args, make_run_dir, write_command_line
from benchlib.serialio import PROBE_BAUDS, ROLE_TEXT, list_serial_ports, probe_port


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        epilog=__doc__.split("\n\n", 1)[1],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--probe-all", action="store_true", help="listen on every USB serial port, not only unidentified ones")
    ap.add_argument("--no-probe", action="store_true", help="VID/PID only, never open a port")
    ap.add_argument("--listen", type=float, default=2.5, help="seconds to listen at each baud while probing (default 2.5)")
    ap.add_argument("--bauds", default=",".join(map(str, PROBE_BAUDS)),
                    help="comma-separated bauds to try while probing (default: %(default)s)")
    ap.add_argument("--all-ttys", action="store_true", help="also list non-USB ttys (Bluetooth, debug console)")
    ap.add_argument("--json", action="store_true", help="print JSON instead of a table")
    add_log_args(ap)
    args = ap.parse_args()
    bauds = [int(b) for b in args.bauds.split(",") if b.strip()]

    ports = list_serial_ports(include_all=args.all_ttys)
    run_dir = make_run_dir(args.log_dir, "ports", args.label)
    write_command_line(run_dir)
    lines: list[str] = []

    def out(s: str = "") -> None:
        lines.append(s)
        if not args.json:
            print(s)

    if not ports:
        out("No USB serial ports found. Is the board plugged in (and the cable a data cable)?")
    if not args.no_probe:
        targets = [p for p in ports if args.probe_all or p.role is None]
        roles = [p.role for p in ports if p.role]
        targets += [p for p in ports if p.role and roles.count(p.role) > 1 and p not in targets]
        for p in targets:
            out(f"probing {p.device} (listen only, {args.listen:g} s per baud) ...")
            p.probe = probe_port(p.device, listen_s=args.listen, bauds=bauds, log=out)

    out()
    out(f"{'device':<34} {'usb id':<10} {'role':<16} evidence")
    out("-" * 100)
    for p in ports:
        role = p.final_role or "unknown"
        ev = []
        if p.role:
            ev.append(f"VID/PID: {p.role_note}")
        elif p.vid is not None:
            ev.append(f"VID/PID not known ({p.description})")
        if p.probe:
            ev.append(f"content @ {p.probe.baud}: {p.probe.evidence}" if p.probe.baud else f"content: {p.probe.evidence}")
            if p.role and p.probe.role and p.probe.role != p.role:
                ev.append("NOTE: content disagrees with VID/PID; trust the content")
        out(f"{p.device:<34} {p.usb_id:<10} {role:<16} {'; '.join(ev)}")
    out()

    console = [p for p in ports if p.final_role == "esp32-console"]
    receiver = [p for p in ports if p.final_role == "um982-com2"]
    for role, hits in (("esp32-console", console), ("um982-com2", receiver)):
        if len(hits) == 1:
            out(f"{ROLE_TEXT[role]:<34} {hits[0].device}")
        elif not hits:
            out(f"{ROLE_TEXT[role]:<34} not found")
        else:
            out(f"{ROLE_TEXT[role]:<34} ambiguous: {', '.join(p.device for p in hits)}")
    if console and receiver and len(console) == 1 and len(receiver) == 1:
        out()
        out("Next:")
        out(f"  uv run capture.py --console {console[0].device} --receiver {receiver[0].device}")
        out(f"  uv run um982.py --port {receiver[0].device} query")

    report = {
        "ports": [
            {
                "device": p.device, "usb_id": p.usb_id, "serial_number": p.serial_number,
                "location": p.location, "description": p.description, "manufacturer": p.manufacturer,
                "role_by_usb_id": p.role, "role": p.final_role,
                "probe": None if not p.probe else {"role": p.probe.role, "baud": p.probe.baud,
                                                     "evidence": p.probe.evidence, "tried": p.probe.tried},
            }
            for p in ports
        ],
        "esp32_console": [p.device for p in console],
        "um982_com2": [p.device for p in receiver],
    }
    (run_dir / "ports.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    (run_dir / "ports.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(f"\nlog: {run_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
