# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""The boot accounting of tools/qemu/smoke.py (also e2e.py's): how each boot on a console ended, and the
QEMU artefact of a planned restart run on the APP CPU told apart from other restarts nobody asked for;
and the watch on slow requests (slow-requests.log) that replaces retries and longer timeouts."""

from __future__ import annotations

import http.server
import importlib.util
import os
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _load_smoke():
    spec = importlib.util.spec_from_file_location("smoke_boot_under_test", ROOT / "tools" / "qemu" / "smoke.py")
    mod = importlib.util.module_from_spec(spec)
    before = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec.loader.exec_module(mod)
    finally:
        sys.dont_write_bytecode = before
    return mod


smoke = _load_smoke()

ROM = "ESP-ROM:esp32s3-20210327\nBuild:Mar 27 2021\n"
POWER_ON = "rst:0x1 (POWERON),boot:0x4 (SPI_FLASH_BOOT)\n"
SW_CPU = "rst:0xc (RTC_SW_CPU_RST),boot:0x4 (SPI_FLASH_BOOT)\n"
BOOTLOADER = ("I (37) boot: ESP-IDF v6.1 2nd stage bootloader\nI (39) boot: compile time Oct 13 2026 11:41:32\n"
              "I (40) boot: Multicore bootloader\nI (47) boot: chip revision: v0.3\n")
LOADED = "I (102) boot.esp32s3: SPI Flash Size : 16MB\nI (242) boot: Loaded app from partition at offset 0x30000\n"
APP = "I (340) cpu_start: Multicore app\nI (423) cpu_start: Pro cpu start user code\nI (492) main_task: Started on CPU0\n"
PLANNED = "W (6767) lifecycle: rebooting (reason 1) from lifecycle, pinned to core 0\n"


def test_one_boot_per_planned_restart():
    text = ROM + POWER_ON + BOOTLOADER + LOADED + APP + PLANNED + ROM + SW_CPU + BOOTLOADER + LOADED + APP
    r = smoke.boot_report(text)
    assert (r.boots, r.unplanned, r.app_cpu, r.off_pro) == (2, [], [], [])
    assert smoke.boot_text(r) == "0 restart(s) without the firmware's own reboot line"
    assert smoke.runner_text(r, 1) == "every planned restart (1) ran on a task pinned to core 0"


def test_a_planned_restart_off_the_pro_cpu_is_named_even_when_it_booted_once():
    # The web server's worker (unpinned) leaving safe mode: scheduling decides the core, so one clean boot
    # proves nothing; the reboot line's runner does.
    off = "W (9100) lifecycle: rebooting (reason 10) from web_io, pinned to no core\n"
    text = (ROM + POWER_ON + BOOTLOADER + LOADED + APP + PLANNED + ROM + SW_CPU + BOOTLOADER + LOADED + APP + off
            + ROM + SW_CPU + BOOTLOADER + LOADED + APP)
    r = smoke.boot_report(text)
    assert (r.boots, r.unplanned, r.app_cpu) == (3, [], [])
    assert r.off_pro == ["reason 10 from web_io, pinned to no core"]
    t = smoke.runner_text(r, 2)
    assert t.startswith("1 planned restart(s) not on a task pinned to the PRO CPU: reason 10 from web_io")
    # A reboot line without its runner (an older image) is named too.
    old = "W (6767) lifecycle: rebooting (reason 1)\n"
    r = smoke.boot_report(ROM + POWER_ON + APP + old + ROM + SW_CPU + APP)
    assert r.off_pro == ["lifecycle: rebooting (reason 1)"]


def test_a_planned_restart_cut_short_in_the_bootloader_is_named():
    # The CI shape: after the planned restart a second rst:0xc banner inside the 2nd-stage bootloader.
    text = (ROM + POWER_ON + BOOTLOADER + LOADED + APP + PLANNED
            + ROM + SW_CPU + BOOTLOADER
            + ROM + SW_CPU + BOOTLOADER + LOADED + APP)
    r = smoke.boot_report(text)
    assert (r.boots, r.unplanned, r.app_cpu) == (3, [2], [2])
    t = smoke.boot_text(r)
    assert t.startswith("1 restart(s) without the firmware's own reboot line (1 of them")
    assert "APP CPU" in t and "no CPU stall" in t


def test_a_crash_is_not_taken_for_the_artefact():
    # The app ran, then a panic and a restart: a crash, whatever reset reason the next banner shows.
    panic = "Guru Meditation Error: Core  1 panic'ed (LoadProhibited).\nRebooting...\n"
    text = ROM + POWER_ON + BOOTLOADER + LOADED + APP + ROM + SW_CPU + BOOTLOADER + LOADED + APP + panic + ROM + SW_CPU
    r = smoke.boot_report(text)
    assert (r.boots, r.unplanned, r.app_cpu) == (3, [1, 2], [])
    # A restart in the bootloader without a planned restart before it: not the artefact either.
    text = ROM + POWER_ON + BOOTLOADER + ROM + SW_CPU + BOOTLOADER + LOADED + APP
    r = smoke.boot_report(text)
    assert (r.unplanned, r.app_cpu) == ([1], [])
    # A watchdog reset in the bootloader after a planned restart (another reset reason): not the artefact.
    text = ROM + POWER_ON + BOOTLOADER + LOADED + APP + PLANNED + ROM + SW_CPU + BOOTLOADER + ROM + \
        "rst:0x10 (RTCWDT_RTC_RST),boot:0x4 (SPI_FLASH_BOOT)\n" + BOOTLOADER + LOADED + APP
    r = smoke.boot_report(text)
    assert (r.unplanned, r.app_cpu) == ([2], [])


def test_qemu_pid_from_run_log(tmp_path):
    log = tmp_path / "run.log"
    log.write_text("Web UI:  http://localhost:8080/\nQEMU pid 4242\n")
    assert smoke.qemu_pid_from(log) == 4242
    log.write_text("Web UI only\n")
    assert smoke.qemu_pid_from(log) is None
    assert smoke.qemu_pid_from(tmp_path / "missing.log") is None


class _Slow(http.server.BaseHTTPRequestHandler):
    delay = 1.0

    def do_GET(self):  # noqa: N802 - http.server's name
        if self.path == "/slow":
            time.sleep(self.delay)
        body = b"ok"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def test_a_slow_request_is_watched(tmp_path, monkeypatch):
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Slow)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    port = srv.server_address[1]
    console = tmp_path / "console.log"
    console.write_text("I (1) old: before the request\n")
    run_log = tmp_path / "run.log"
    run_log.write_text(f"QEMU pid {os.getpid()}\n")
    monkeypatch.setattr(smoke, "SLOW_S", 0.3)
    monkeypatch.setattr(smoke, "_slow_watch", None)
    smoke.watch_requests(tmp_path, console, run_log, port, time.monotonic())
    try:
        assert smoke.request(port, "GET", "/", timeout=5)[0] == 200   # quick: not watched
        assert smoke.slow_requests() == 0

        def later():
            time.sleep(0.5)
            with open(console, "a") as f:
                f.write("W (2) web: printed while the request waited\n")
        threading.Thread(target=later, daemon=True).start()
        assert smoke.request(port, "GET", "/slow", timeout=5)[0] == 200
    finally:
        srv.shutdown()
    assert smoke.slow_requests() == 1
    log = (tmp_path / "slow-requests.log").read_text()
    assert "GET /slow: no answer after 0 s, watching" in log
    assert "GET /slow: probe GET / on another connection: HTTP 200" in log
    assert f"GET /slow: QEMU threads (pid {os.getpid()}): " in log
    assert "GET /slow: console: W (2) web: printed while the request waited" in log
    assert "old: before the request" not in log
    assert "GET /slow: HTTP 200 after" in log
    assert "GET / " not in log.replace("probe GET / ", "")


def _pcap(segments: list[tuple[str, int, str, int, int, int, int]]) -> bytes:
    """A pcap (Ethernet, IPv4) of TCP segments (src ip, src port, dst ip, dst port, flags, seq, ack)."""
    out = bytearray(bytes.fromhex("d4c3b2a1") + (2).to_bytes(2, "little") + (4).to_bytes(2, "little")
                    + bytes(8) + (65535).to_bytes(4, "little") + (1).to_bytes(4, "little"))
    for src, sport, dst, dport, flags, seq, ack in segments:
        tcp = (sport.to_bytes(2, "big") + dport.to_bytes(2, "big") + seq.to_bytes(4, "big") + ack.to_bytes(4, "big")
               + bytes([0x50, flags]) + (5840).to_bytes(2, "big") + bytes(4))
        ip = (bytes([0x45, 0]) + (20 + len(tcp)).to_bytes(2, "big") + bytes(4) + bytes([64, 6]) + bytes(2)
              + bytes(int(x) for x in src.split(".")) + bytes(int(x) for x in dst.split(".")))
        frame = bytes(12) + b"\x08\x00" + ip + tcp
        out += bytes(8) + len(frame).to_bytes(4, "little") + len(frame).to_bytes(4, "little") + frame
    return bytes(out)


def test_what_the_station_acknowledged_from_its_packets(tmp_path):
    g, h = smoke.GUEST_IP, "10.0.2.2"
    isn = 0xFFFFFF00   # the client's sequence numbers wrap inside the connection
    pcap = tmp_path / "net.pcap"
    pcap.write_bytes(_pcap([
        (h, 51000, g, 80, 0x02, isn, 0),                              # SYN
        (g, 80, h, 51000, 0x12, 7000, isn + 1),                       # SYN-ACK
        (g, 80, h, 52000, 0x10, 1, 99),                               # another connection
        (g, 80, h, 51000, 0x18, 7001, (isn + 1 + 3000) % (1 << 32)),  # answer, 3000 bytes acked
        (g, 80, h, 51000, 0x11, 7300, (isn + 1 + 16490) % (1 << 32)),  # FIN, all of it and the client's FIN
    ]))
    segs = smoke.tcp_from_guest(pcap, 51000)
    assert [s.flags for s in segs] == [0x12, 0x18, 0x11]
    assert smoke.acked_by_guest(segs) == 16490
    # A station that stopped reading: its acknowledgements end a window into the body, then a reset.
    pcap.write_bytes(_pcap([
        (g, 80, h, 51001, 0x12, 7000, 101),
        (g, 80, h, 51001, 0x10, 7001, 101 + 5760),
        (g, 80, h, 51001, 0x14, 7301, 101 + 5761),
    ]))
    segs = smoke.tcp_from_guest(pcap, 51001)
    assert smoke.acked_by_guest(segs) == 5761 and any(s.flags & smoke.TCP_RST for s in segs)
    assert smoke.acked_by_guest([]) is None
    assert smoke.tcp_from_guest(tmp_path / "missing.pcap", 1) == []
    (tmp_path / "bad.pcap").write_bytes(b"not a pcap")
    assert smoke.tcp_from_guest(tmp_path / "bad.pcap", 1) == []


def test_who_closed_first_from_the_stations_packets():
    syn = smoke.Segment(0x12, 9000, 501)                     # SYN-ACK: the client's ISN was 500
    first = [syn, smoke.Segment(0x18, 9001, 501 + 60), smoke.Segment(0x11, 9200, 501 + 60)]
    assert smoke.guest_closed_first(first, 60) is True        # its FIN acknowledges the request only
    second = [syn, smoke.Segment(0x18, 9001, 501 + 60), smoke.Segment(0x11, 9200, 501 + 61)]
    assert smoke.guest_closed_first(second, 60) is False      # the client's FIN came first (LAST_ACK)
    assert smoke.guest_closed_first([syn], 60) is None
    assert smoke.guest_closed_first([], 60) is None


def test_connections_counted_by_their_syn_ack_isn():
    # Two connections from one client port; the station sends the first SYN-ACK again (the client's ACK was
    # dropped), which carries the same initial sequence number and is not a third connection.
    first = [smoke.Segment(0x12, 9000, 501), smoke.Segment(0x12, 9000, 501), smoke.Segment(0x18, 9001, 561),
             smoke.Segment(0x11, 9200, 561)]
    assert smoke.synack_isns(first) == [9000]
    both = first + [smoke.Segment(0x12, 44000, 7001), smoke.Segment(0x12, 44000, 7001),
                    smoke.Segment(0x18, 44001, 7061)]
    assert smoke.synack_isns(both) == [9000, 44000]
    # The port still in TIME_WAIT: a bare ACK (or a reset) to the second SYN, no SYN-ACK.
    held = first + [smoke.Segment(0x10, 9201, 562)]
    assert smoke.synack_isns(held) == [9000]
    assert smoke.synack_isns([]) == []
