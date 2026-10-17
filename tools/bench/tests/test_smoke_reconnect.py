# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""tools/qemu/smoke.py's account of a client back on its port inside the station's TIME_WAIT: the request's
time taken apart in qemu-net.pcap (libslirp's SYN resend against the station's own part), and the watch on
slow requests counting such a request as explained, not as a slow one nothing accounts for."""

from __future__ import annotations

import http.server
import importlib.util
import socket
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _load_smoke():
    spec = importlib.util.spec_from_file_location("smoke_reconnect_under_test", ROOT / "tools" / "qemu" / "smoke.py")
    mod = importlib.util.module_from_spec(spec)
    before = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec.loader.exec_module(mod)
    finally:
        sys.dont_write_bytecode = before
    return mod


smoke = _load_smoke()

G, H, P = "10.0.2.15", "10.0.2.2", 55519


def _pcap(packets: list[tuple[float, str, int, str, int, int, int, int, int]]) -> bytes:
    """A pcap (Ethernet, IPv4) of TCP segments (time, src ip, src port, dst ip, dst port, flags, seq, ack,
    payload bytes)."""
    out = bytearray(bytes.fromhex("d4c3b2a1") + (2).to_bytes(2, "little") + (4).to_bytes(2, "little")
                    + bytes(8) + (65535).to_bytes(4, "little") + (1).to_bytes(4, "little"))
    for t, src, sport, dst, dport, flags, seq, ack, n in packets:
        tcp = (sport.to_bytes(2, "big") + dport.to_bytes(2, "big") + seq.to_bytes(4, "big") + ack.to_bytes(4, "big")
               + bytes([0x50, flags]) + (5840).to_bytes(2, "big") + bytes(4) + bytes(n))
        ip = (bytes([0x45, 0]) + (20 + len(tcp)).to_bytes(2, "big") + bytes(4) + bytes([64, 6]) + bytes(2)
              + bytes(int(x) for x in src.split(".")) + bytes(int(x) for x in dst.split(".")))
        frame = bytes(12) + b"\x08\x00" + ip + tcp
        sec, usec = int(t), round((t - int(t)) * 1e6)
        out += (sec.to_bytes(4, "little") + usec.to_bytes(4, "little") + len(frame).to_bytes(4, "little")
                + len(frame).to_bytes(4, "little") + frame)
    return bytes(out)


# A reconnect from an earlier run's qemu-net.pcap (client port 55519), as it was there: the first connection
# (the station closes first), then 1 s later the same port: the SYN turned away with a bare ACK, libslirp's
# reset, its resend 5.97 s after the first SYN, the SYN-ACK, the request, the answer and the station's FIN.
FIRST = [
    (71.407868, H, P, G, 80, 0x02, 45376001, 0, 0),
    (71.408272, G, 80, H, P, 0x12, 1508966326, 45376002, 0),
    (71.408291, H, P, G, 80, 0x10, 45376002, 1508966327, 0),
    (71.409414, H, P, G, 80, 0x18, 45376002, 1508966327, 71),
    (71.411525, G, 80, H, P, 0x18, 1508966327, 45376073, 70),
    (71.413897, G, 80, H, P, 0x19, 1508966397, 45376073, 409),
    (71.413923, H, P, G, 80, 0x10, 45376073, 1508966807, 0),
    (71.414193, H, P, G, 80, 0x11, 45376073, 1508966807, 0),
    (71.414670, G, 80, H, P, 0x10, 1508966807, 45376074, 0),
]
AGAIN = [
    (72.458324, H, P, G, 80, 0x02, 45632001, 0, 0),
    (72.458951, G, 80, H, P, 0x10, 1508966807, 45376074, 0),          # the bare ACK: TIME_WAIT
    (72.458967, H, P, G, 80, 0x04, 45376074, 0, 0),                   # libslirp's reset with RCV.NXT
    (78.430654, H, P, G, 80, 0x02, 45632001, 0, 0),                   # its resend, on its own timer
    (78.431691, G, 80, H, P, 0x12, 1510722076, 45632002, 0),
    (78.431786, H, P, G, 80, 0x10, 45632002, 1510722077, 0),
    (78.432364, H, P, G, 80, 0x18, 45632002, 1510722077, 71),
    (78.533656, G, 80, H, P, 0x18, 1510722077, 45632073, 70),
    (78.534758, G, 80, H, P, 0x19, 1510722147, 45632073, 409),
    (78.534773, H, P, G, 80, 0x10, 45632073, 1510722557, 0),
]


def test_packets_carry_their_time_and_payload(tmp_path):
    pcap = tmp_path / "net.pcap"
    pcap.write_bytes(_pcap(FIRST + [(71.5, G, 80, H, 40000, 0x10, 1, 1, 0)] + AGAIN))
    packets = smoke.tcp_packets(pcap, P)
    assert len(packets) == len(FIRST) + len(AGAIN)
    assert [round(p.t, 6) for p in packets[:3]] == [71.407868, 71.408272, 71.408291]
    assert [p.payload for p in packets[3:6]] == [71, 70, 409]
    assert smoke.tcp_segments(pcap, P) == [(p.from_guest, p.seg) for p in packets]


def test_the_reconnect_time_is_libslirps_resend_and_milliseconds_of_the_station(tmp_path):
    pcap = tmp_path / "net.pcap"
    pcap.write_bytes(_pcap(FIRST + AGAIN))
    second = smoke.tcp_packets(pcap, P)[len(FIRST):]
    t = smoke.reconnect_time(second)
    assert t is not None
    assert round(t.resend, 3) == 5.972                      # libslirp's first resend, 12 ticks of 500 ms
    assert round(t.turned * 1000, 1) == 0.6                 # the station's bare ACK to the first SYN
    assert round(t.synack * 1000, 1) == 1.0                 # its SYN-ACK to the resend
    assert round(t.answer * 1000) == 102                    # the request to its FIN
    assert round(t.station, 3) == 0.104
    assert smoke.RESEND_MIN_S <= t.resend <= smoke.RESEND_MAX_S and t.station < smoke.SLOW_S
    # The SYN taken at once (nothing turned away): nothing to take apart.
    assert smoke.reconnect_time(smoke.tcp_packets(pcap, P)[:len(FIRST)]) is None
    # No answer to the SYN it took: the station's part is not in the pcap.
    pcap.write_bytes(_pcap(FIRST + AGAIN[:5]))
    assert smoke.reconnect_time(smoke.tcp_packets(pcap, P)[len(FIRST):]) is None
    assert smoke.reconnect_time([]) is None


def test_a_station_that_held_the_request_shows_in_its_own_part(tmp_path):
    # The same reconnect with the answer 4.9 s late: the total stays under the old 12 s, but the station's
    # own part is what the check holds to SLOW_S.
    late = [x if x[0] < 78.5 else (x[0] + 4.9,) + x[1:] for x in AGAIN]
    pcap = tmp_path / "net.pcap"
    pcap.write_bytes(_pcap(FIRST + late))
    t = smoke.reconnect_time(smoke.tcp_packets(pcap, P)[len(FIRST):])
    assert round(t.resend, 3) == 5.972 and round(t.station, 3) == 5.004 and t.station >= smoke.SLOW_S


class _Slow(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/slow":
            time.sleep(0.6)
        body = b"ok"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def test_a_slow_request_its_packets_explain_is_counted_apart(tmp_path, monkeypatch):
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Slow)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    port = srv.server_address[1]
    (tmp_path / "console.log").write_text("")
    (tmp_path / "run.log").write_text("")
    monkeypatch.setattr(smoke, "SLOW_S", 0.3)
    monkeypatch.setattr(smoke, "_slow_watch", None)
    smoke.watch_requests(tmp_path, tmp_path / "console.log", tmp_path / "run.log", port, time.monotonic())
    try:
        watched = []
        for path in ("/slow", "/slow", "/"):
            with socket.create_connection(("127.0.0.1", port), timeout=5) as c, \
                    smoke.Watched(f"GET {path}") as w:
                w.port(c)
                c.sendall(f"GET {path} HTTP/1.0\r\n\r\n".encode())
                while c.recv(4096):
                    pass
                w.outcome = "HTTP 200"
            watched.append(w)
    finally:
        srv.shutdown()
    watched[0].explain("6.08 s, 5.97 s of it libslirp's SYN resend")
    watched[2].explain("never said: it was not slow")
    assert [w.slow for w in watched] == [True, True, False]
    assert smoke.slow_requests() == 2
    told = smoke.slow_explained()
    assert len(told) == 1 and told[0].startswith("GET /slow (client port ")
    assert told[0].endswith(": 6.08 s, 5.97 s of it libslirp's SYN resend")
    log = (tmp_path / "slow-requests.log").read_text()
    assert told[0] in log and "never said" not in log
