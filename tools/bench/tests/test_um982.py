# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""um982.py against scripted ports (no hardware): it sends nothing to a
receiver whose loader it hears, and its advice after no answer is to listen
first, never a search over other bauds."""

from __future__ import annotations

import json
import threading
import time

import um982
from benchlib import unicore
from benchlib.stream import crc32_unicore
from benchlib.um982_sim import Um982Responder
from benchlib.unicore import UnicoreSession
from receiver_25164 import GROUP_BODIES, LINES, group_log


class ScriptedPort:
    """The part of pyserial um982.py uses. `chatter` is sent again and again while the port is open (a loader
    that shows its prompt); every write is recorded."""

    def __init__(self, baud: int, chatter: bytes = b"", every: float = 0.1, timeout: float = 0.05):
        self.baudrate = baud
        self.timeout = timeout
        self.chatter = chatter
        self.every = every
        self.rx = bytearray()
        self.written: list[bytes] = []
        self.lock = threading.Lock()
        self.closed = False
        self.next_at = 0.0

    def _tick(self) -> None:
        now = time.monotonic()
        if self.chatter and now >= self.next_at:
            self.next_at = now + self.every
            with self.lock:
                self.rx += self.chatter

    @property
    def in_waiting(self) -> int:
        self._tick()
        with self.lock:
            return len(self.rx)

    def read(self, n: int = 1) -> bytes:
        self._tick()
        with self.lock:
            if not self.rx:
                empty = True
            else:
                empty = False
                out = bytes(self.rx[:n])
                del self.rx[:n]
        if empty:
            time.sleep(self.timeout)
            return b""
        return out

    def write(self, data: bytes) -> int:
        self.written.append(bytes(data))
        return len(data)

    def flush(self) -> None:
        pass

    def close(self) -> None:
        self.closed = True


LOADER = b"\r\n  5 Menu cmd\r\n  6 Soft reset cpu\r\nboot>"


def opener_for(ports: list, chatter_at: dict[int, bytes] | None = None):
    def opener(device, baud, timeout=0.05):
        p = ScriptedPort(baud, (chatter_at or {}).get(baud, b""), timeout=timeout)
        ports.append(p)
        return p
    return opener


def run(tmp_path, *argv, opener):
    return um982.main(["--port", "/dev/fake-com2", "--log-dir", str(tmp_path / "logs"), *argv], opener=opener)


def test_loader_text():
    assert um982.loader_heard(b"\xff\xffboot>")
    assert um982.loader_heard(b"  6 Soft reset cpu\r\n")
    assert um982.loader_heard(b"uart baudrate: 460800\r\n")
    assert not um982.loader_heard(b"$GNGGA,123519,4807.038,N*47\r\n#VERSIONA,79,GPS;...")
    assert not um982.loader_heard(b"")


def test_loader_in_the_listen_window_sends_nothing(tmp_path, capsys):
    ports: list[ScriptedPort] = []
    code = run(tmp_path, "--listen-first", "0.3", opener=opener_for(ports, {115200: LOADER}))
    assert code == um982.EXIT_LOADER == 3
    assert len(ports) == 1 and ports[0].closed
    assert ports[0].written == []   # not even the bare line end
    out = capsys.readouterr().out
    assert "loader was heard at 115200" in out and "Nothing was sent" in out and "power-cycle" in out
    assert "find-baud" not in out
    run_dir = next((tmp_path / "logs").glob("*-um982*"))
    assert "boot>" in (run_dir / "session-rx.bin").read_bytes().decode("latin-1")
    assert "loader was heard" in (run_dir / "summary.txt").read_text()


def test_send_is_refused_too_at_the_loader(tmp_path, capsys):
    ports: list[ScriptedPort] = []
    code = run(tmp_path, "send", "VERSIONA", opener=opener_for(ports, {115200: LOADER}))
    assert code == um982.EXIT_LOADER
    assert ports[0].written == []


def test_no_versiona_answer_advises_listening_first(tmp_path, capsys, monkeypatch):
    monkeypatch.setattr(um982, "DAY_ONE", ["VERSIONA"])   # one query keeps the silent wait short
    ports: list[ScriptedPort] = []
    code = run(tmp_path, opener=opener_for(ports))
    assert code == um982.EXIT_OK
    sent = b"".join(ports[0].written)
    assert sent.replace(b"\r\n", b"") == b"VERSIONA" * 2   # the bare line end, then VERSIONA and its one retry
    out = capsys.readouterr().out
    assert "No answer to VERSIONA" in out
    assert "find-baud" not in out
    assert "capture.py --receiver /dev/fake-com2 --receiver-baud 460800 --duration 20" in out
    assert "run neither um982.py nor bootprobe.py" in out and "still silent: stop" in out


def test_find_baud_stops_at_a_loader_heard_while_listening(capsys):
    ports: list[ScriptedPort] = []
    out = um982.Out()
    found = um982.find_baud("/dev/fake-com2", out, opener=opener_for(ports, {460800: LOADER}), listen_s=0.3)
    assert found == -460800
    # Listened at 115200, then heard the loader at 460800: no other baud opened, the ask phase never ran.
    assert [p.baudrate for p in ports] == [115200, 460800]
    assert all(p.written == [] for p in ports)
    assert any("loader" in line for line in out.lines)


def test_find_baud_asks_only_after_listening_everywhere(capsys):
    # A silent receiver: every baud listened to first, then VERSIONA at each (the search as it was).
    ports: list[ScriptedPort] = []
    out = um982.Out()
    found = um982.find_baud("/dev/fake-com2", out, opener=opener_for(ports), listen_s=0.05)
    assert found is None
    n = len(um982.FIND_BAUDS)
    assert len(ports) == 2 * n
    assert all(p.written == [] for p in ports[:n])
    assert all(p.written == [b"\r\nVERSIONA\r\n"] for p in ports[n:])


def test_main_with_find_baud_at_a_loader_exits_3(tmp_path, capsys, monkeypatch):
    real = um982.find_baud
    monkeypatch.setattr(um982, "find_baud", lambda dev, out, opener, **kw: real(dev, out, opener=opener, listen_s=0.2,
                                                                                **kw))
    ports: list[ScriptedPort] = []
    code = run(tmp_path, "--find-baud", opener=opener_for(ports, {115200: LOADER}))
    assert code == um982.EXIT_LOADER
    assert len(ports) == 1 and ports[0].written == []
    assert "loader was heard at 115200" in capsys.readouterr().out


def test_advice_for_a_receiver_at_its_loader_never_names_bootprobe():
    # bootprobe cannot leave a loader the receiver already sits at: it only listens 2 s (a prompt printed
    # earlier is not heard), then sends VERSIONA at the application baud. Power-cycle only.
    steps = um982.post_failure_steps("/dev/fake-com2")
    loader_step = next(s for s in steps if "boot>" in s)
    assert "power-cycle" in loader_step
    assert "bootprobe" in loader_step  # named only to rule it out
    assert "neither um982.py nor bootprobe.py" in loader_step
    assert "sends 6" not in " ".join(steps)
    assert "sends 6" not in (um982.__doc__ or "") and "(or bootprobe" not in (um982.__doc__ or "")
    assert "bootprobe" not in " ".join(um982.loader_advice("/dev/fake-com2", 460800, sent_at=[]))


class LoaderAfterQuery(ScriptedPort):
    """A receiver that drops to its loader once VERSIONA reached it at this baud."""

    def write(self, data: bytes) -> int:
        n = super().write(data)
        if b"VERSIONA" in data:
            with self.lock:
                self.rx += LOADER
        return n


def test_find_baud_at_a_loader_in_the_ask_phase_says_what_was_sent(tmp_path, capsys, monkeypatch):
    real = um982.find_baud
    monkeypatch.setattr(um982, "find_baud", lambda dev, out, opener, **kw: real(dev, out, opener=opener, listen_s=0.05,
                                                                                **kw))
    ports: list[ScriptedPort] = []
    target = um982.FIND_BAUDS[3]

    def opener(device, baud, timeout=0.05):
        cls = LoaderAfterQuery if baud == target and len(ports) >= len(um982.FIND_BAUDS) else ScriptedPort
        p = cls(baud, timeout=timeout)
        ports.append(p)
        return p

    code = run(tmp_path, "--find-baud", opener=opener)
    assert code == um982.EXIT_LOADER
    asked = [p.baudrate for p in ports[len(um982.FIND_BAUDS):] if p.written]
    assert asked == list(um982.FIND_BAUDS[:4])
    out = capsys.readouterr().out
    assert "nothing sent" not in out and "Nothing was sent" not in out
    for b in asked:
        assert str(b) in out
    assert "VERSIONA was sent at" in out
    summary = next((tmp_path / "logs").glob("*-um982*")) / "summary.txt"
    assert "Nothing was sent" not in summary.read_text()


# -- a receiver that answers: the replay's scripted UM982 behind COM2 ----------------------------------------------

def obsvm_empty() -> str:
    """One epoch with no observations (no antenna)."""
    text = "OBSVMA,97,GPS,UNKNOWN,1,1000,25164,0,18,975;0"
    return f"#{text}*{crc32_unicore(text.encode()):08x}"


class ReceiverPort(ScriptedPort):
    """COM2 of a UM982 build 25164 without an antenna: the replay's scripted receiver (benchlib.um982_sim) for
    identity, configuration, signal groups and restarts, plus the one-shot logs it does not have (ANT1GROUPA and
    ANT2GROUPA with the bodies the bench saw, an empty sky). banner=False loses the restart banner."""

    def __init__(self, baud: int, responder: Um982Responder, timeout: float = 0.05, banner: bool = True):
        super().__init__(baud, timeout=timeout)
        self.resp = responder
        self.banner = banner
        self.pending = b""

    def _tick(self) -> None:
        data = self.resp.due()
        if data and not self.banner:
            data = b"".join(x for x in data.splitlines(keepends=True) if not x.startswith(b"$devicename"))
        if data:
            with self.lock:
                self.rx += data

    def _answer(self, cmd: str) -> list[str]:
        master, slave = GROUP_BODIES[self.resp.signal_group()]
        data = {"ANT1GROUPA": group_log("ANT1GROUPA", master), "ANT2GROUPA": group_log("ANT2GROUPA", slave),
                "GPGSV": LINES["no satellites"], "OBSVMA": obsvm_empty()}[cmd.upper()]
        return [unicore.ack_line(cmd), data]

    def write(self, data: bytes) -> int:
        n = super().write(data)
        self.pending += data
        while b"\n" in self.pending:
            line, self.pending = self.pending.split(b"\n", 1)
            cmd = line.strip().decode()
            if cmd.upper() in ("ANT1GROUPA", "ANT2GROUPA", "GPGSV", "OBSVMA") and not self.resp.restarting():
                with self.lock:
                    self.rx += "".join(x + "\r\n" for x in self._answer(cmd)).encode()
            else:
                self.resp.feed(line + b"\n")
        return n


def receiver_opener(ports: list, responder: Um982Responder, banner: bool = True):
    def opener(device, baud, timeout=0.05):
        p = ReceiverPort(baud, responder, timeout=timeout, banner=banner)
        ports.append(p)
        return p
    return opener


def run_result(tmp_path) -> dict:
    return json.loads((next((tmp_path / "logs").glob("*-um982*")) / "summary.json").read_text())


def test_query_reads_the_signals_the_group_sets_up(tmp_path, capsys):
    ports: list[ScriptedPort] = []
    code = run(tmp_path, "--listen-first", "0.3", opener=receiver_opener(ports, Um982Responder(group=(3, 6))))
    assert code == um982.EXIT_OK
    sent = b"".join(ports[0].written)
    # One-shots: sent alone, never with a period (that would start a periodic log).
    assert b"\r\nANT1GROUPA\r\n" in sent and b"\r\nANT2GROUPA\r\n" in sent and b"GROUPA 1" not in sent
    out = capsys.readouterr().out
    assert "master: group 3" in out and "slave: group 6" in out
    assert "Galileo  E1, E5a, E5b, E6" in out
    assert "signal group 3 sets up 4 bands on the master antenna for: Galileo" in out
    s = run_result(tmp_path)["summary"]
    assert s["signal_group"] == [3, 6] and s["four_band_set_up"] == ["Galileo"]
    assert s["group_signals"]["master"]["systems"]["Galileo"]["bands"] == ["L1", "L2", "L5", "L6"]
    assert s["group_signals"]["slave"]["four_band"] == []
    assert s["four_band_systems"] == []       # nothing tracked without an antenna
    replies = next((tmp_path / "logs").glob("*-um982*")) / "replies"
    assert any("ANT1GROUPA" in p.name for p in replies.iterdir())


def test_send_decodes_the_group_queries(tmp_path, capsys):
    ports: list[ScriptedPort] = []
    code = run(tmp_path, "--listen-first", "0.3", "send", "ANT1GROUPA", "ant2groupa",
               opener=receiver_opener(ports, Um982Responder(group=(7, 0))))
    assert code == um982.EXIT_OK            # read-only: no --write, nothing to confirm
    out = capsys.readouterr().out
    assert "BeiDou   B1I, B2I, B3I, B1C, B2a, B2b" in out
    assert "slave: group 0, the slave antenna is off" in out
    replies = run_result(tmp_path)["replies"]
    assert replies[0]["decoded"]["four_band"] == ["BeiDou"] and replies[1]["decoded"]["off"] is True


def test_e6_check_waits_out_the_restart_and_confirms_e6_without_sky(tmp_path, capsys, monkeypatch):
    # The banner is lost and the banner wait ends early, so VERSIONA polls reach the receiver while it restarts.
    resp = Um982Responder(group=(4, 5), restart_s=2.0)
    monkeypatch.setattr(um982, "BANNER_WAIT_S", 0.5)
    confirmed: list[str] = []

    def confirm(out, port, baud, cmds):
        confirmed.extend(cmds)
        return cmds != ["CONFIG SIGNALGROUP 4 5"]   # keep 3 6: no restore

    monkeypatch.setattr(um982, "confirm", confirm)
    ports: list[ScriptedPort] = []
    code = run(tmp_path, "--listen-first", "0.3", "e6-check", "--write", "--observe", "1", "--every", "1",
               opener=receiver_opener(ports, resp, banner=False))
    assert code == um982.EXIT_OK
    out = capsys.readouterr().out
    assert confirmed == ["CONFIG SIGNALGROUP 3 6", "CONFIG SIGNALGROUP 4 5"]
    assert "ANT1GROUPA   (read-only" in out                      # in the plan
    assert "still restarting" in out and "The Board is resetting" in out and "receiver is back" in out
    assert "set up: Galileo E6 yes, BeiDou B2a no" in out
    res = run_result(tmp_path)["e6_check"]
    assert res["group_before"] == [4, 5] and res["group_after"] == [3, 6]
    assert res["set_up_before"]["four_band"] == [] and res["set_up_after"]["four_band"] == ["Galileo"]
    assert "E6" not in res["set_up_before"]["systems"]["Galileo"]["signals"]
    sent = b"".join(ports[0].written)
    assert b"GROUPA 1" not in sent and sent.count(b"CONFIG SIGNALGROUP") == 1
    # Polled once a second while it restarted, not resent at once on its "resetting" answer.
    polls = [t for t, c, a in resp.log if c == "VERSIONA" and a == "restarting"]
    assert polls and all(b - a >= 0.9 for a, b in zip(polls, polls[1:]))
    assert "kept 3 6" in out and resp.signal_group() == (3, 6)


def test_a_restarting_receiver_is_reported_and_nothing_is_resent(tmp_path, capsys):
    resp = Um982Responder(group=(4, 5), restart_s=30.0)
    resp.feed(b"RESET\r\n")
    ports: list[ScriptedPort] = []
    code = run(tmp_path, "--listen-first", "0.3", "send", "VERSIONA", opener=receiver_opener(ports, resp))
    assert code == um982.EXIT_OK
    assert b"".join(ports[0].written).count(b"VERSIONA") == 1    # read-only, yet no resend
    out = capsys.readouterr().out
    assert "receiver restarting" in out and "send it again once it is back" in out
    assert LINES["restarting"] in out
    log = (next((tmp_path / "logs").glob("*-um982*")) / "session.log").read_text()
    assert "RX[restarting] The Board is resetting" in log


def test_query_of_a_restarting_receiver_says_so(tmp_path, capsys):
    resp = Um982Responder(group=(4, 5), restart_s=30.0)
    resp.feed(b"RESET\r\n")
    ports: list[ScriptedPort] = []
    code = run(tmp_path, "--listen-first", "0.3", opener=receiver_opener(ports, resp))
    assert code == um982.EXIT_OK
    out = capsys.readouterr().out
    assert "it is restarting. Run the query again" in out and "No answer to VERSIONA" not in out
    sent = b"".join(ports[0].written)
    assert sent.count(b"VERSIONA") == 1


def test_a_refused_query_ends_at_its_acknowledgement(tmp_path):
    # No data follows a refusal: the session does not wait out max_data for it.
    port = ReceiverPort(115200, Um982Responder())
    sess = UnicoreSession(port, tmp_path)
    try:
        t0 = time.monotonic()
        r = sess.command("UNILOGLIST", max_data=8.0)
        assert r.ok is False and r.reason == "PARSING FAILED NO MATCHING FUNC  UNILOGLIST" and not r.data
        assert time.monotonic() - t0 < 2.0
    finally:
        sess.close()
