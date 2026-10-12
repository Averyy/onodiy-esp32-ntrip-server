#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Talk to the UM982 from the PC over its COM2 USB port ("UM982 UART2", CH340N).

READ-ONLY BY DEFAULT. Without --write the tool sends only these queries,
which report state and change nothing:
  CR LF        a bare line end before the first command (ends any half-typed input)
  VERSIONA     identity: model, firmware build, PN-serial, board ID, compile date
  CONFIG       configuration dump (signal group, port bauds, RTCM options, antenna, ...)
  MODE         operating mode (rover/base, survey-in or fixed)
  MASK         elevation, constellation and C/N0 masks
  UNILOGLIST   logs enabled on every port (what COM1 sends to the ESP32)
  BESTNAVA     one position solution (no rate: output once)
  GPGSV        satellites in view with signal IDs, once (shows E6 / B2a if tracked)
  OBSVMA       one epoch of raw observations, one entry per tracked signal, once
`send` also accepts these read-only one-shots without --write:
  BESTNAVXYZA BESTSATA GPGGA GPGSA GPRMC GPGST AGCA RTKSTATUSA SATSINFOA
  SATELLITEA BASEPOSA BASEINFOA RECTIMEA JAMSTATUSA RTCMSTATUSA
--find-baud listens at each candidate baud, then sends VERSIONA at each until
one answers: only for a receiver whose application is known to run; never
after a failed or interrupted receiver update or a bootprobe run that did not
end with exit 0 (its queries at many bauds could reach a loader menu, where
garbled bytes may read as menu keys). There, listen first (below).

Nothing is sent at all when the listen-first window (or the baud search's
listening) hears the receiver's loader ("boot>" or its menu text): the
receiver waits at its loader, and only a power-cycle (or bootprobe, which
sends "6" only at a menu that numbers it) brings it back. Exit status 3.

After a failed or interrupted receiver update, or a bootprobe run that did
not end with exit 0, or when the receiver does not answer:
  1. listen only: uv run capture.py --receiver <port> --receiver-baud 460800 --duration 20,
     and the same with --receiver-baud 115200;
  2. "boot>" seen: the receiver is at its loader: power-cycle it (or run
     bootprobe, which sends 6 only at a menu that numbers it); never um982.py
     at that baud;
  3. application output seen at a baud: uv run um982.py --port <port> --baud <that baud>;
  4. nothing at either: power-cycle, then step 1 again; still silent: stop.

Anything else changes the receiver and needs --write; the tool prints the
exact bytes first and sends only after you type "yes". Never sent at all,
not even with --write: FRESET, RESET with parameters, bootloader keys
("T@", bare menu numbers) or any firmware/bootloader command. A change made
here is not saved by itself, but the station's next configuration of the
receiver (its first apply, a profile change, a re-apply after drift, the
recovery ladder's reconfigure step) ends with SAVECONFIG, which saves the
whole running configuration, MASK and MMP changes included: undo what
should not stay.

Subcommands:
  query            day-one set above, every reply saved verbatim, then a summary (default)
  send CMD ...     send commands one by one (read-only ones freely, others with --write)
  e6-check         is a 4-band constellation tracked? Read-only part first; with --write:
                   set the signal group (default "3 6", Galileo E6), wait for the receiver
                   to restart, watch GPGSV/OBSVMA (and RTCM 1097 with --msm) for E6, then
                   offer to restore the previous group

Examples:
  uv run um982.py --port auto
  uv run um982.py --port /dev/cu.usbserial-1120 send BESTNAVA
  uv run um982.py --port auto send --write "RTCM1077 COM2 1" "UNLOG COM2 RTCM1077"
  uv run um982.py --port auto e6-check                  # shows the plan, sends only queries
  uv run um982.py --port auto e6-check --write --group "7 0"
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from pathlib import Path

import serial

from benchlib import rtcm
from benchlib.common import StopFlag, add_log_args, make_run_dir, write_command_line
from benchlib.serialio import find_port, open_serial
from benchlib.stream import StreamSplitter
from benchlib.unicore import (
    DAY_ONE, READ_ONLY, SignalView, UnicoreSession, blocked_reason, four_band_systems, is_read_only,
    normalise, parse_bestnav, parse_config, parse_gsv, parse_mode, parse_obsvm, parse_versiona,
    signal_group, warnings_for,
)

CONFIG_KEYS = ("SIGNALGROUP", "COM1", "COM2", "COM3", "RTCMB1CB2A", "BASEANTENNAMODEL", "MMP", "UNDULATION",
               "ALLEPHRTCM", "RTCMOUTPUT", "RTCMCLOCKOFFSET", "SBAS", "CMDFORMAT")

EXIT_OK = 0
EXIT_NOT_CONFIRMED = 1
EXIT_NO_PORT = 2       # cannot open the port, or no baud answered
EXIT_LOADER = 3        # the receiver's loader was heard: nothing was sent

# The receiver's loader: its prompt (no line end) and words of its banner and menu.
LOADER_PROMPT = b"boot>"
LOADER_WORDS = (b"soft reset", b"efuse from uart", b"default action:", b"uart baudrate:", b"load os")
LISTEN_MIN_S = 1.0     # the listen-first window is never shorter: it is the loader check


def loader_heard(data: bytes) -> bool:
    """Bytes that only the receiver's loader prints: its prompt, or its banner and menu words."""
    low = bytes(data).lower()
    return LOADER_PROMPT in low or any(w in low for w in LOADER_WORDS)


def post_failure_steps(port: str = "<port>") -> list[str]:
    """What to do after a failed or interrupted receiver update, a bootprobe run that did not end with exit 0,
    or a receiver that does not answer: listen first, send nothing to a receiver that may sit at its loader."""
    return [
        "What to do (listen first; send nothing until the application is heard):",
        f"  1. listen only: `uv run capture.py --receiver {port} --receiver-baud 460800 --duration 20`, "
        "and the same with --receiver-baud 115200;",
        "  2. \"boot>\" seen: the receiver is at its loader: power-cycle it (or run bootprobe, which sends 6 only "
        "at a menu that numbers it); never um982.py at that baud;",
        f"  3. application output seen at a baud: `uv run um982.py --port {port} --baud <that baud>`;",
        "  4. nothing at either: power-cycle, then step 1 again; still silent: stop.",
    ]


def loader_advice(port: str, baud: int) -> list[str]:
    return [
        f"The receiver's loader was heard at {baud} (\"boot>\" or its menu): it waits at its loader. "
        "Nothing was sent.",
        "What to do: power-cycle the receiver (unplug the board: both USB-C cables), then listen first "
        f"(`uv run capture.py --receiver {port} --receiver-baud 460800 --duration 20`, and at 115200) and use "
        "um982.py only at a baud where the application is heard.",
    ]


class Out:
    """Print and keep the text for summary.txt."""

    def __init__(self) -> None:
        self.lines: list[str] = []

    def __call__(self, s: str = "") -> None:
        self.lines.append(s)
        print(s, flush=True)


def confirm(out: Out, port: str, baud: int, cmds: list[str]) -> bool:
    out(f"\nAbout to send to {port} @ {baud} (each line followed by CR LF):")
    for c in cmds:
        out(f"    {c}")
        for w in warnings_for(c):
            out(f"        note: {w}")
    if not sys.stdin.isatty():
        out("No terminal to confirm on: nothing sent.")
        return False
    try:
        ans = input("Type 'yes' to send (anything else cancels): ")
    except (EOFError, KeyboardInterrupt):
        ans = ""
    if ans.strip().lower() != "yes":
        out("Not confirmed: nothing sent.")
        return False
    return True


def save_reply(run_dir: Path, n: int, reply) -> Path:
    replies = run_dir / "replies"
    replies.mkdir(exist_ok=True)
    name = re.sub(r"[^A-Za-z0-9]+", "_", reply.command)[:40]
    path = replies / f"{n:02d}-{name}.txt"
    path.write_text("".join(line + "\r\n" for line in reply.lines), encoding="latin-1")
    return path


def describe_reply(reply) -> str:
    if not reply.answered:
        return f"no answer within the timeout ({reply.sends} send(s))"
    ack = reply.reason if reply.ack else "no ack"
    return f"{ack}; {len(reply.data)} data line(s) in {reply.elapsed:.2f} s"


FIND_BAUDS = (115200, 460800, 921600, 230400, 57600, 38400, 19200, 9600)


def find_baud(device: str, out: Out, opener=open_serial, listen_s: float = 2.0) -> int | None:
    """Listen at each baud, then ask VERSIONA at each (the baud search, only when asked for).

    The listen phase stops the whole search at the loader's text: then
    nothing is sent at all (the ask phase never runs) and the result is
    -baud (the baud it was heard at)."""
    for phase in ("listen", "ask"):
        for baud in FIND_BAUDS:
            ser = opener(device, baud, timeout=0.05)
            try:
                sp = StreamSplitter()
                if phase == "ask":
                    ser.write(b"\r\nVERSIONA\r\n")
                    ser.flush()
                end = time.time() + (listen_s if phase == "listen" else 2.0)
                items = []
                raw = bytearray()
                while time.time() < end:
                    data = ser.read(4096)
                    if data:
                        raw += data
                        items += sp.feed(data)
                if loader_heard(raw):
                    out(f"  {phase} @ {baud}: the receiver's loader (\"boot>\" or its menu): stopped, nothing sent")
                    return -baud
                good = [i for i in items if i.kind == "rtcm" or (i.kind == "text" and i.check not in ("bad", "none"))]
                out(f"  {phase} @ {baud}: {len(good)} valid item(s)")
                if good:
                    return baud
            finally:
                ser.close()
    return None


def run_query(sess: UnicoreSession, run_dir: Path, out: Out, cmds: list[str], counter: list[int]) -> dict:
    replies = {}
    for cmd in cmds:
        counter[0] += 1
        retries = 1 if is_read_only(cmd) else 0
        max_data = 8.0 if cmd in ("CONFIG", "OBSVMA", "UNILOGLIST") else 4.0
        reply = sess.command(cmd, ack_timeout=3.0, quiet=0.7, max_data=max_data, retries=retries)
        path = save_reply(run_dir, counter[0], reply)
        out(f"  {cmd:<12} {describe_reply(reply)}  -> {path.relative_to(run_dir)}")
        replies[cmd] = reply
    return replies


def summarise(replies: dict, out: Out) -> dict:
    s: dict = {}
    r = replies.get("VERSIONA")
    if r and r.data:
        line = r.data[-1]
        v = parse_versiona(line.text)
        v["crc_ok"] = line.check == "crc32"
        s["version"] = v
        out("\n== Receiver (VERSIONA, CRC " + ("ok" if v["crc_ok"] else "NOT ok") + ")")
        if "error" in v:
            out(f"   could not parse: {v['error']}")
        else:
            out(f"   model {v['model']}  firmware {v['firmware']} (build {v['build']})  compiled {v['compile_date']}")
            out(f"   PN {v['pn']}  serial {v['serial']}  board ID {v['efuse_id']}  auth {v['authorisation']}")
            out(f"   header has {v['header_fields']} fields")
            if v["model"] != "UM982":
                out("   NOTE: the station firmware only configures and flashes a receiver that reports UM982")
    else:
        out("\n== Receiver: no VERSIONA reply (wrong port or baud? listen first, as below)")
    for key in ("CONFIG", "MASK"):
        r = replies.get(key)
        if not (r and r.data):
            continue
        cfg = parse_config([d.text for d in r.data])
        s[key.lower()] = cfg
        if key == "CONFIG":
            out(f"\n== Configuration (CONFIG, {len(cfg)} items, {len(r.data)} lines)")
            for k in CONFIG_KEYS:
                if k in cfg:
                    out(f"   {k:<17} {' | '.join(cfg[k])}")
            sg = signal_group(cfg)
            s["signal_group"] = sg
            if sg:
                hint = {(4, 5): "factory default: no E6 and no B2a -> max 3 bands (see e6-check)",
                        (3, 6): "Galileo E6 group", (7, 0): "BeiDou B2a group (base only)"}.get(sg, "")
                out(f"   -> signal group {sg[0]} {sg[1] if sg[1] is not None else ''}  {hint}")
            others = [k for k in cfg if k not in CONFIG_KEYS]
            if others:
                out(f"   other items: {', '.join(others)}")
        else:
            out("\n== Masks (MASK)")
            for k, vals in cfg.items():
                out(f"   {k}: {' | '.join(vals)}")
    r = replies.get("MODE")
    if r and r.data:
        s["mode"] = parse_mode(r.data[-1].text)
        out(f"\n== Mode: {s['mode']}")
    r = replies.get("UNILOGLIST")
    if r and r.data:
        logs = [d.text.lstrip("<").strip() for d in r.data if d.text.startswith("<")]
        logs = [x for x in logs if x and not x.isdigit()]  # the first `<` line is the count
        s["logs"] = logs
        out(f"\n== Logs enabled (UNILOGLIST): {len(logs)} line(s)")
        for line in logs:
            out(f"   {line}")
    r = replies.get("BESTNAVA")
    if r and r.data:
        b = parse_bestnav(r.data[-1].text)
        s["bestnav"] = b
        if "lat_deg" in b:
            out(f"\n== Position (BESTNAVA): {b['sol_status']} {b['pos_type']}  lat {b['lat_deg']:.8f} "
                f"lon {b['lon_deg']:.8f}  h(MSL) {b['height_msl_m']:.3f} m  sats {b['sats_used']}/{b['sats_tracked']}")
    views = []
    r = replies.get("GPGSV")
    if r and r.data:
        views.append(parse_gsv([d.text for d in r.data]))
    r = replies.get("OBSVMA")
    if r and r.data:
        views.append(parse_obsvm(r.data[-1].text))
    for view in views:
        out(f"\n== Signals tracked ({view.source}; C/N0 > 0 only counts toward bands)")
        for line in view.lines():
            out(f"   {line}")
        s[f"signals_{view.source.lower()}"] = view.to_dict()
    if views:
        four = sorted(set().union(*(four_band_systems(v) for v in views)))
        s["four_band_systems"] = four
        out("\n== Verdict")
        if four:
            out(f"   4 bands tracked on: {', '.join(four)} -> band reward 0.95 possible")
        else:
            out("   no constellation shows 4 bands right now -> try `um982.py e6-check --write` "
                "(group 3 6 for Galileo E6, or --group \"7 0\" for BeiDou B2a)")
    return s


def observe_once(sess: UnicoreSession, run_dir: Path, counter: list[int]) -> list[SignalView]:
    views = []
    for cmd in ("GPGSV", "OBSVMA"):
        counter[0] += 1
        reply = sess.command(cmd, ack_timeout=3.0, quiet=0.7, max_data=8.0, retries=1)
        save_reply(run_dir, counter[0], reply)
        if reply.data:
            views.append(parse_gsv([d.text for d in reply.data]) if cmd == "GPGSV" else parse_obsvm(reply.data[-1].text))
    return views


def msm_view(frames) -> SignalView:
    view = SignalView("RTCM MSM")
    for it in frames:
        payload = rtcm.payload_of(it.data)
        mt = rtcm.msg_type(payload)
        if mt is None or not rtcm.msm_system(mt) or rtcm.is_suspect_sbas(mt):
            continue
        try:
            m = rtcm.decode_msm(payload)
        except ValueError:
            continue
        for c in m.cells:
            view.add(m.system, rtcm.signal_label(m.system, c.sig), rtcm.band_of(m.system, c.sig), str(c.sat), c.cnr)
    return view


def wait_restart(sess: UnicoreSession, sent_at: float, out: Out, stop: StopFlag) -> bool:
    """Listen for the restart banner (<= 45 s), then poll VERSIONA once a second (<= 25 s)."""
    out("   waiting for the receiver to restart (listening only, up to 45 s) ...")
    t_banner = None
    while time.time() - sent_at < 45 and not stop.is_set():
        hits = [b for b in sess.banners if b.t and b.t >= sent_at]
        if hits:
            t_banner = hits[0].t
            out(f"   restart banner {hits[0].text!r} {t_banner - sent_at:.1f} s after the command")
            break
        time.sleep(0.2)
    if t_banner is None:
        out("   no restart banner seen (the port may print it at another baud); polling VERSIONA anyway")
    start = time.time()
    while time.time() - start < 25 and not stop.is_set():
        r = sess.command("VERSIONA", ack_timeout=1.0, quiet=0.3, max_data=1.0)
        if r.data:
            out(f"   VERSIONA answered {time.time() - sent_at:.1f} s after the command: receiver is back")
            return True
    out("   receiver did not answer VERSIONA within 25 s of the banner wait")
    return False


def set_group(sess, out, args, group: tuple[int, int], stop, label: str) -> bool:
    cmd = f"CONFIG SIGNALGROUP {group[0]} {group[1]}"
    if not confirm(out, args.port, args.baud, [cmd]):
        return False
    sent = time.time()
    r = sess.command(cmd, ack_timeout=5.0, retries=0)
    out(f"   {cmd}: {describe_reply(r)}")
    if r.ack and not r.ok:
        out(f"   the receiver rejected it ({label}); nothing changed")
        return False
    return wait_restart(sess, sent, out, stop)


def e6_check(sess, run_dir: Path, out: Out, args, stop: StopFlag, counter: list[int]) -> dict:
    res: dict = {}
    out("== Read-only part: identity, current signal group, signals tracked now")
    replies = run_query(sess, run_dir, out, ["VERSIONA", "CONFIG"], counter)
    cfg = parse_config([d.text for d in replies["CONFIG"].data]) if replies["CONFIG"].data else {}
    prev = signal_group(cfg)
    res["group_before"] = prev
    if replies["VERSIONA"].data:
        v = parse_versiona(replies["VERSIONA"].data[-1].text)
        res["version"] = v
        out(f"   receiver {v.get('model')} {v.get('firmware')}")
    out(f"   current signal group: {prev if prev else 'unknown (CONFIG gave no SIGNALGROUP line)'}")
    views = observe_once(sess, run_dir, counter)
    for view in views:
        for line in view.lines():
            out(f"   [{view.source}] {line}")
    now4 = sorted(set().union(*(four_band_systems(v) for v in views))) if views else []
    out(f"   4-band constellations now: {', '.join(now4) or 'none'}")
    res["four_band_before"] = now4

    target = tuple(int(x) for x in args.group.split())
    if len(target) != 2:
        raise SystemExit("--group takes two numbers, e.g. \"3 6\"")
    plan = []
    if prev != target:
        plan.append(f"CONFIG SIGNALGROUP {target[0]} {target[1]}   (receiver restarts, group saved by itself)")
    if args.msm:
        plan.append("RTCM1097 COM2 1   (Galileo MSM7 on this port, not saved)")
    plan.append(f"then every {args.every:g} s for up to {args.observe:g} s: GPGSV and OBSVMA once each (read-only)")
    if args.msm:
        plan.append("UNLOG COM2 RTCM1097   (stop it again)")
    if prev != target and prev is not None:
        plan.append(f"offer: CONFIG SIGNALGROUP {prev[0]} {prev[1] if prev[1] is not None else 0}   (restore, asked separately)")
    out("\n== Plan" + ("" if args.write else " (dry run: add --write to do it)"))
    for p in plan:
        out(f"   {p}")
    if not args.write:
        return res
    if prev is None and not args.force_unknown:
        out("   current group unknown, so it could not be restored afterwards; add --force-unknown to go on")
        return res

    if prev != target:
        out(f"\n== Setting signal group {target[0]} {target[1]}")
        if not set_group(sess, out, args, target, stop, "signal group change"):
            return res
        counter[0] += 1
        r = sess.command("CONFIG", max_data=8.0, retries=1)
        save_reply(run_dir, counter[0], r)
        now = signal_group(parse_config([d.text for d in r.data]))
        res["group_after"] = now
        out(f"   signal group now: {now}" + ("" if now == target else "   (NOT the requested group)"))
    else:
        out("\n== Already in the requested group: observing only")

    msm_on = False
    if args.msm:
        cmd = "RTCM1097 COM2 1"
        if confirm(out, args.port, args.baud, [cmd]):
            r = sess.command(cmd, retries=0)
            out(f"   {cmd}: {describe_reply(r)}")
            msm_on = bool(r.ok)

    out(f"\n== Observing (up to {args.observe:g} s; Ctrl-C ends early)")
    start = time.time()
    frames_from = len(sess.frames)
    seen_rounds = 0
    union: dict[str, set] = {}
    first_e6 = None
    while time.time() - start < args.observe and not stop.is_set():
        views = observe_once(sess, run_dir, counter)
        # MSM frames on COM2: from --msm, or already streaming there.
        mv = msm_view(sess.frames[frames_from:])
        frames_from = len(sess.frames)
        if mv.signals:
            views.append(mv)
        e6 = []
        for v in views:
            for system in v.signals:
                union.setdefault(system, set()).update(v.band_list(system))
            if "L6" in v.band_list("Galileo"):
                e6.append(v.source)
        four = sorted(set().union(*(four_band_systems(v) for v in views))) if views else []
        t = time.time() - start
        out(f"   t+{t:5.0f} s  E6: {'yes (' + ', '.join(e6) + ')' if e6 else 'no'}   "
            f"4-band: {', '.join(four) or 'none'}   " +
            "  ".join(f"{s}:{'/'.join(v.band_list(s))}" for v in views[:1] for s in sorted(v.signals)))
        if e6 and first_e6 is None:
            first_e6 = t
        seen_rounds = seen_rounds + 1 if (e6 or four) else 0
        if seen_rounds >= 2 and not args.full:
            out("   seen in two rounds in a row: done (use --full to keep watching)")
            break
        stop.wait(max(0.0, args.every - (time.time() - start - t)))
    if stop.is_set():
        out("   watch ended by Ctrl-C")
        stop.clear()  # the clean-up steps below must still be offered
    res["bands_seen"] = {s: sorted(b) for s, b in union.items()}
    res["e6_first_seen_s"] = first_e6
    res["four_band_after"] = sorted(s for s, b in union.items() if len(b & {"L1", "L2", "L5", "L6"}) >= 4)
    out("\n== Result")
    out(f"   Galileo E6: {'seen ' + format(first_e6, '.0f') + ' s into the watch' if first_e6 is not None else 'NOT seen'}")
    for s, b in sorted(res["bands_seen"].items()):
        out(f"   {s:<8} bands seen: {'/'.join(x for x in ('L1', 'L2', 'L5', 'L6', 'E5ab') if x in b)}")
    out(f"   4-band constellations: {', '.join(res['four_band_after']) or 'none'}")

    if msm_on:
        cmd = "UNLOG COM2 RTCM1097"
        if confirm(out, args.port, args.baud, [cmd]):
            r = sess.command(cmd, retries=0)
            out(f"   {cmd}: {describe_reply(r)}")
        else:
            out("   RTCM1097 stays on COM2 until the next restart (it was not saved)")
    if prev is not None and prev != target and res.get("group_after") == target:
        restore = (prev[0], prev[1] if prev[1] is not None else 0)
        out(f"\n== Restore the previous signal group {restore[0]} {restore[1]}?")
        if set_group(sess, out, args, restore, stop, "restore"):
            counter[0] += 1
            r = sess.command("CONFIG", max_data=8.0, retries=1)
            save_reply(run_dir, counter[0], r)
            out(f"   signal group now: {signal_group(parse_config([d.text for d in r.data]))}")
        else:
            out(f"   kept {target[0]} {target[1]} (it is saved in the receiver)")
    return res


def main(argv: list[str] | None = None, opener=open_serial) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        epilog=__doc__.split("\n\n", 1)[1],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--port", default="auto", help="UM982 COM2 port, or 'auto' (CH340 bridge by VID/PID; default)")
    ap.add_argument("--baud", type=int, default=115200, help="port baud (default 115200, the receiver's factory rate)")
    ap.add_argument("--find-baud", action="store_true",
                    help="listen at each baud, then send VERSIONA (read-only) at each until one answers: only for a "
                         "receiver whose application is known to run; never after a failed or interrupted receiver "
                         "update or a bootprobe run that did not end with exit 0")
    ap.add_argument("--listen-first", type=float, default=2.0,
                    help="seconds to listen before sending anything, to log what the port streams and to stop at "
                         "the receiver's loader (default 2, at least 1)")
    add_log_args(ap)
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("query", help="day-one read-only query set (default)")
    sp = sub.add_parser("send", help="send commands; anything outside the read-only list needs --write")
    sp.add_argument("commands", nargs="+", metavar="CMD", help="one command per argument (quote commands with spaces)")
    sp.add_argument("--write", action="store_true", help="allow commands that change the receiver (each is confirmed)")
    ep = sub.add_parser("e6-check", help="check for a 4-band constellation; with --write try a signal group")
    ep.add_argument("--write", action="store_true", help="actually change the signal group (confirmed first)")
    ep.add_argument("--group", default="3 6", help='signal group to test, "3 6" (Galileo E6, default) or "7 0" (BeiDou B2a)')
    ep.add_argument("--observe", type=float, default=180.0, help="seconds to watch for the signals (default 180)")
    ep.add_argument("--every", type=float, default=15.0, help="seconds between observation rounds (default 15)")
    ep.add_argument("--full", action="store_true", help="watch the whole --observe time even once E6 shows up")
    ep.add_argument("--msm", action="store_true", help="also enable RTCM1097 on COM2 during the watch (unsaved; removed after)")
    ep.add_argument("--force-unknown", action="store_true", help="go on even if the current group cannot be read")
    args = ap.parse_args(argv)
    args.cmd = args.cmd or "query"

    if args.cmd == "send":
        for c in args.commands:
            why = blocked_reason(c)
            if why:
                ap.error(f"{c!r}: {why}")
            if not is_read_only(c) and not args.write:
                ap.error(f"{c!r} is not on the read-only list ({', '.join(READ_ONLY)}); add --write to send it")

    if args.port == "auto":
        dev = find_port("um982-com2")
        if not dev:
            ap.error("--port auto: no single CH340 (UM982 COM2) bridge found by VID/PID; run `uv run ports.py`")
        args.port = dev

    run_dir = make_run_dir(args.log_dir, "um982", args.label or args.cmd)
    write_command_line(run_dir)
    out = Out()
    stop = StopFlag()
    out(f"um982 {args.cmd} on {args.port} @ {args.baud}; log: {run_dir}")

    if args.cmd == "send":
        # Confirm before the port is even opened: nothing at all goes out unconfirmed.
        to_confirm = [normalise(c) for c in args.commands if not is_read_only(c)]
        if to_confirm and not confirm(out, args.port, args.baud, to_confirm):
            (run_dir / "summary.txt").write_text("\n".join(out.lines) + "\n", encoding="utf-8")
            return EXIT_NOT_CONFIRMED

    def done(code: int, lines: list[str]) -> int:
        for line in lines:
            out(line)
        (run_dir / "summary.txt").write_text("\n".join(out.lines) + "\n", encoding="utf-8")
        return code

    if args.find_baud:
        out("finding the baud (listen first, then VERSIONA at each candidate):")
        found = find_baud(args.port, out, opener=opener)
        if found is not None and found < 0:
            return done(EXIT_LOADER, loader_advice(args.port, -found))
        if not found:
            return done(EXIT_NO_PORT, ["no baud answered; check the cable/port (uv run ports.py) and that the "
                                       "receiver is powered"] + post_failure_steps(args.port))
        out(f"receiver answers at {found}")
        args.baud = found

    try:
        ser = opener(args.port, args.baud, timeout=0.05)
    except (serial.SerialException, OSError) as e:
        out(f"cannot open {args.port}: {e}")
        return EXIT_NO_PORT
    sess = UnicoreSession(ser, run_dir)
    result: dict = {"port": args.port, "baud": args.baud, "command": args.cmd}
    counter = [0]
    code = EXIT_OK
    try:
        listen_s = max(args.listen_first, LISTEN_MIN_S)
        items = sess.listen(listen_s)
        kinds: dict[str, int] = {}
        for it in items:
            key = f"RTCM {it.rtcm_type}" if it.kind == "rtcm" else (
                it.text.split(",", 1)[0][:12] if it.kind == "text" else it.kind)
            kinds[key] = kinds.get(key, 0) + 1
        desc = ", ".join(f"{k} x{v}" for k, v in sorted(kinds.items())) or "nothing (quiet port)"
        out(f"listened {listen_s:g} s before sending: {desc}")
        result["unsolicited"] = kinds
        # The loader's prompt has no line end: look at the bytes themselves (the session keeps them all).
        sess.rx_raw.flush()
        heard = (run_dir / "session-rx.bin").read_bytes()
        if loader_heard(heard):
            result["loader_heard"] = True
            for line in loader_advice(args.port, args.baud):
                out(line)
            code = EXIT_LOADER
            return code
        sess.write_raw("\r\n")
        time.sleep(0.2)
        if args.cmd == "query":
            out("\n== Day-one queries (read-only)")
            replies = run_query(sess, run_dir, out, DAY_ONE, counter)
            result["summary"] = summarise(replies, out)
            if not replies["VERSIONA"].answered:
                out("\nNo answer to VERSIONA. Check the port (uv run ports.py), and do not probe other bauds on a "
                    "receiver that may sit at its loader.")
                for line in post_failure_steps(args.port):
                    out(line)
        elif args.cmd == "send":
            result["replies"] = []
            for c in args.commands:
                counter[0] += 1
                ro = is_read_only(c)
                r = sess.command(c, retries=1 if ro else 0, max_data=8.0)
                path = save_reply(run_dir, counter[0], r)
                out(f"\n> {normalise(c)}   [{describe_reply(r)}]  -> {path.relative_to(run_dir)}")
                for line in r.lines:
                    out(f"  {line}")
                result["replies"].append({"command": r.command, "ack": r.reason, "lines": r.lines})
                if not ro and not r.answered:
                    out("  no acknowledgement: not resent automatically (state-changing); check the receiver")
        elif args.cmd == "e6-check":
            result["e6_check"] = e6_check(sess, run_dir, out, args, stop, counter)
    finally:
        sess.close()
        ser.close()
        (run_dir / "summary.txt").write_text("\n".join(out.lines) + "\n", encoding="utf-8")
        (run_dir / "summary.json").write_text(json.dumps(result, indent=2, default=str) + "\n", encoding="utf-8")
        print(f"\nlog: {run_dir}  (session.log has every byte sent and received)")
    return code


if __name__ == "__main__":
    sys.exit(main())
