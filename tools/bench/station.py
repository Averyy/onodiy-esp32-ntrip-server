#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Read our station firmware's web API over Wi-Fi/LAN (components/web/API.md).

Read-only: besides logging in and out (POST/DELETE /api/session) it only
sends GET requests. It never changes settings, reboots or starts updates.

The admin password comes from $STATION_PASSWORD or a prompt; it is never
written to the logs. The host is --host or $STATION_HOST: the station's IP
address, its mDNS name (onodiy-1a2b.local until it is renamed: the setup
AP's four characters), or 192.168.4.1 when joined to its setup AP.

Subcommands:
  dump    save status, receiver, signals, sky, casters, Wi-Fi, system, update,
          receiver-update session, health (with boot history and core dump
          summary), availability, task stats, the event journal, the log ring
          of this and the previous boot, and the core dump if there is one (default)
  watch   one compact status line per second (Ctrl-C to stop); --logs also
          tails the device log ring
  get     GET one API path and print the JSON (e.g. station.py get /api/gnss/signals)

Examples:
  STATION_HOST=192.168.1.50 uv run station.py dump
  uv run station.py --host onodiy-1a2b.local watch --logs
  uv run station.py get /api/health
"""

from __future__ import annotations

import argparse
import getpass
import json
import os
import sys
import time
from pathlib import Path

import httpx

from benchlib.common import StopFlag, add_log_args, iso, make_run_dir, write_command_line

DUMP_ENDPOINTS = [
    ("session", "/api/session"),
    ("status", "/api/status"),
    ("gnss", "/api/gnss"),
    ("gnss-signals", "/api/gnss/signals"),
    ("sky", "/api/sky"),
    ("gnss-flash", "/api/gnss/flash"),
    ("casters", "/api/casters"),
    ("local-caster", "/api/local-caster"),
    ("wifi", "/api/wifi"),
    ("system", "/api/system"),
    ("update", "/api/update"),
    ("health", "/api/health"),
    ("availability", "/api/availability"),
    ("diag-tasks", "/api/diag/tasks"),
    ("upload-gnss", "/api/upload/gnss"),
]


class ApiError(Exception):
    def __init__(self, status: int, body):
        self.status = status
        self.body = body
        code = body.get("error", {}).get("code") if isinstance(body, dict) else None
        super().__init__(f"HTTP {status}" + (f" {code}" if code else ""))


class Station:
    def __init__(self, host: str, password: str | None, timeout: float = 8.0):
        self.host = host
        self.password = password
        self.client = httpx.Client(base_url=f"http://{host}", timeout=timeout,
                                   headers={"User-Agent": "onodiy-bench"})
        self.logged_in = False

    def _password(self) -> str:
        if self.password is None:
            self.password = os.environ.get("STATION_PASSWORD") or getpass.getpass(f"admin password for {self.host}: ")
        return self.password

    def login(self) -> None:
        r = self.client.post("/api/session", json={"password": self._password()})
        body = _json(r)
        if r.status_code != 200:
            raise ApiError(r.status_code, body)
        self.logged_in = True

    def logout(self) -> None:
        if self.logged_in:
            try:
                self.client.delete("/api/session")
            except httpx.HTTPError:
                pass
            self.logged_in = False

    def get(self, path: str, params=None, relogin: bool = True):
        r = self.client.get(path, params=params)
        if r.status_code == 401 and relogin:
            self.login()
            r = self.client.get(path, params=params)
        if r.status_code >= 400:
            raise ApiError(r.status_code, _json(r))
        ctype = r.headers.get("content-type", "")
        return _json(r) if "json" in ctype else r.content

    def close(self) -> None:
        self.logout()
        self.client.close()


def _json(r: httpx.Response):
    try:
        return r.json()
    except ValueError:
        return {"raw": r.text[:2000]}


def dump(st: Station, run_dir: Path, args) -> int:
    api = run_dir / "api"
    api.mkdir()
    results = {}
    for name, path in DUMP_ENDPOINTS:
        try:
            data = st.get(path)
            (api / f"{name}.json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
            results[name] = data
            print(f"  {path:<22} ok")
        except ApiError as e:
            (api / f"{name}.error.json").write_text(json.dumps(e.body, indent=2) + "\n", encoding="utf-8")
            print(f"  {path:<22} {e}")
        except httpx.HTTPError as e:
            print(f"  {path:<22} {type(e).__name__}: {e}")

    # Log rings (this boot, previous boot): page until `more` is false.
    for boot, fname in ((None, "log-this-boot.txt"), ("prev", "log-prev-boot.txt")):
        text, cursor, gaps, pages = [], 0, 0, 0
        try:
            while pages < 2000:
                params = {"cursor": cursor}
                if boot:
                    params["boot"] = boot
                page = st.get("/api/logs", params=params)
                pages += 1
                text.append(page.get("text", ""))
                gaps += 1 if page.get("gap") else 0
                if page.get("cursor", cursor) == cursor or not page.get("more"):
                    break
                cursor = page["cursor"]
            (run_dir / fname).write_text("".join(text), encoding="utf-8")
            print(f"  /api/logs{'?boot=prev' if boot else '':<12} {sum(len(t) for t in text)} chars"
                  + (f", {gaps} gap(s)" if gaps else ""))
        except (ApiError, httpx.HTTPError) as e:
            print(f"  /api/logs{'?boot=prev' if boot else ''}  {e}")

    # Event journal: page from the start.
    entries, cursor = [], 0
    try:
        for _ in range(1000):
            page = st.get("/api/journal", params={"cursor": cursor})
            entries += page.get("entries", [])
            if not page.get("more") or page.get("cursor", cursor) == cursor:
                break
            cursor = page["cursor"]
        (run_dir / "journal.json").write_text(json.dumps(entries, indent=2) + "\n", encoding="utf-8")
        print(f"  /api/journal          {len(entries)} entries")
    except (ApiError, httpx.HTTPError) as e:
        print(f"  /api/journal          {e}")

    health = results.get("health") or {}
    if (health.get("coredump") or {}).get("present") and not args.no_coredump:
        try:
            blob = st.get("/api/coredump")
            ver = (results.get("system") or {}).get("version", "unknown")
            path = run_dir / f"coredump-{ver}.elf"
            path.write_bytes(blob if isinstance(blob, bytes) else json.dumps(blob).encode())
            print(f"  /api/coredump         {len(blob)} bytes -> {path.name} "
                  f"(decode: idf.py coredump-info -c {path})  NOTE: may contain secrets")
        except (ApiError, httpx.HTTPError) as e:
            print(f"  /api/coredump         {e}")

    print()
    for line in summary_lines(results):
        print(line)
    (run_dir / "summary.txt").write_text("\n".join(summary_lines(results)) + "\n", encoding="utf-8")
    return 0


def summary_lines(r: dict) -> list[str]:
    out = []
    s, sy, h, g = r.get("status") or {}, r.get("system") or {}, r.get("health") or {}, r.get("gnss") or {}
    dev = s.get("device", {})
    out.append(f"station {dev.get('hostname')} {sy.get('version') or dev.get('version')} "
               f"(idf {sy.get('idf')}, {sy.get('chip', {}).get('model')}) up {fmt_dur(s.get('uptime_s'))}, "
               f"clock {dev.get('time_source')}")
    rx = s.get("receiver", {})
    out.append(f"receiver {rx.get('state')} {rx.get('model')} {rx.get('firmware')} responding={rx.get('responding')} "
               f"applied={rx.get('profile_applied')} rtcm_age={rx.get('rtcm_age_ms')} ms "
               f"best={rx.get('best_sys')}/{rx.get('best_bands')} bands surveying={rx.get('surveying')}")
    link = g.get("link") or {}
    if link:
        out.append(f"link {link.get('mode')} @ {link.get('baud')}  profile signal group "
                   f"{(g.get('profile') or {}).get('signal_group_master')} {(g.get('profile') or {}).get('signal_group_slave')}")
    sig = r.get("gnss-signals") or {}
    for sysd in sig.get("systems") or []:
        out.append(f"  {sysd.get('sys'):<8} sats {sysd.get('satellites')} bands {'/'.join(sysd.get('bands') or [])} "
                   f"cnr {sysd.get('cnr_dbhz')}")
    for c in s.get("casters", []):
        out.append(f"caster {c.get('slot')} {c.get('name')} {c.get('state')} ({c.get('reason')}) lag {c.get('lag_ms_now')} ms "
                   f"online today {c.get('today_online_s')} s offline {c.get('today_offline_s')} s")
    net = s.get("network", {})
    out.append(f"network online={net.get('online')} ssid={net.get('ssid')} rssi={net.get('rssi')} upstream={net.get('upstream')}")
    out.append(f"health {h.get('level')} last_step={h.get('last_step')} alerts={[a.get('id') if isinstance(a, dict) else a for a in s.get('alerts', [])]}")
    boot = h.get("boot") or {}
    out.append(f"boot reset={boot.get('reset_reason')} class={boot.get('reset_class')} crashes={boot.get('crashes')} "
               f"brownouts={boot.get('brownouts_since_power_on')} safe_mode={boot.get('safe_mode')}")
    lk = (h.get("link") or {})
    fr = lk.get("framing") or {}
    if lk:
        out.append(f"gnss link rx={lk.get('rx_bytes')} B overflows={lk.get('hw_fifo_overflows')}/{lk.get('rx_buffer_full')} "
                   f"frames={fr.get('rtcm_frames')} crc_err={fr.get('rtcm_crc_errors')} text={fr.get('text_lines')} "
                   f"discarded={fr.get('bytes_discarded')}")
    cd = h.get("coredump") or {}
    if cd.get("present"):
        out.append(f"CORE DUMP present: task {cd.get('task')} pc {cd.get('pc')}")
    for b in (h.get("boot_history") or [])[:5]:
        out.append(f"  boot #{b.get('boot_no')} {b.get('reset_class')}/{b.get('reset_reason')} "
                   f"prev uptime {fmt_dur(b.get('prev_uptime_s'))} {b.get('version')}")
    return out


def fmt_dur(s) -> str:
    if s is None:
        return "?"
    s = int(s)
    d, s = divmod(s, 86400)
    h, s = divmod(s, 3600)
    m, s = divmod(s, 60)
    return (f"{d}d" if d else "") + f"{h:02d}:{m:02d}:{s:02d}"


def status_line(s: dict) -> str:
    rx = s.get("receiver") or {}
    parts = [f"up {fmt_dur(s.get('uptime_s'))}",
             f"rx {rx.get('state')}",
             f"rtcm {rx.get('rtcm_age_ms')}ms",
             f"best {rx.get('best_sys')}/{rx.get('best_bands')}"]
    if rx.get("surveying"):
        parts.append(f"survey {rx.get('survey_window_left_s')}s left")
    for c in s.get("casters") or []:
        if c.get("enabled", True):
            parts.append(f"c{c.get('slot')} {c.get('state')} lag {c.get('lag_ms_now')}ms")
    net = s.get("network") or {}
    parts.append(f"wifi {net.get('rssi')}dBm{'' if net.get('online') else ' OFFLINE'}")
    hl = (s.get("health") or {}).get("level")
    parts.append(f"health {hl}")
    alerts = [a.get("id") if isinstance(a, dict) else str(a) for a in s.get("alerts") or []]
    if alerts:
        parts.append("alerts " + ",".join(alerts))
    if (s.get("safe_mode") or {}).get("active"):
        parts.append("SAFE MODE")
    return " | ".join(parts)


def watch(st: Station, run_dir: Path, args, stop: StopFlag) -> int:
    status_log = open(run_dir / "status.jsonl", "a", encoding="utf-8")
    watch_log = open(run_dir / "watch.log", "a", encoding="utf-8")
    log_text = open(run_dir / "device-log.txt", "a", encoding="utf-8") if args.logs else None
    cursor = None
    down_since = None
    last = None
    try:
        while not stop.is_set():
            t = time.time()
            try:
                s = st.get("/api/status")
                if down_since is not None:
                    print(f"{time.strftime('%H:%M:%S')} back after {t - down_since:.0f} s", flush=True)
                    down_since = None
                status_log.write(json.dumps({"t": iso(t), "status": s}) + "\n")
                line = status_line(s)
                if last is not None and s.get("uptime_s") is not None and last.get("uptime_s") is not None \
                        and s["uptime_s"] < last["uptime_s"]:
                    line += "   <<< REBOOTED"
                    cursor = None
                last = s
                print(f"{time.strftime('%H:%M:%S')} {line}", flush=True)
                watch_log.write(f"{iso(t)} {line}\n")
                watch_log.flush()
                if log_text is not None:
                    if cursor is None:
                        # Start at the end of the ring: read until `more` is false.
                        cursor = 0
                        while True:
                            page = st.get("/api/logs", params={"cursor": cursor})
                            if page.get("cursor", cursor) == cursor or not page.get("more"):
                                cursor = page.get("cursor", cursor)
                                break
                            cursor = page["cursor"]
                    else:
                        while True:
                            page = st.get("/api/logs", params={"cursor": cursor})
                            text = page.get("text", "")
                            if text:
                                log_text.write(text)
                                log_text.flush()
                                for ln in text.splitlines():
                                    print(f"    | {ln}", flush=True)
                            if page.get("gap"):
                                print("    | [log lines were overwritten before they were read]", flush=True)
                            new = page.get("cursor", cursor)
                            if new == cursor or not page.get("more"):
                                cursor = new
                                break
                            cursor = new
            except (httpx.HTTPError, ApiError) as e:
                if down_since is None:
                    down_since = t
                    print(f"{time.strftime('%H:%M:%S')} unreachable: {e}", flush=True)
                    watch_log.write(f"{iso(t)} unreachable: {e}\n")
                st.logged_in = False
                try:
                    st.login()
                except (httpx.HTTPError, ApiError):
                    pass
            stop.wait(max(0.0, args.interval - (time.time() - t)))
    finally:
        status_log.close()
        watch_log.close()
        if log_text:
            log_text.close()
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        epilog=__doc__.split("\n\n", 1)[1],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--host", default=os.environ.get("STATION_HOST"), required="STATION_HOST" not in os.environ,
                    help="station address (default: $STATION_HOST)")
    ap.add_argument("--timeout", type=float, default=8.0, help="HTTP timeout in seconds (default 8)")
    add_log_args(ap)
    sub = ap.add_subparsers(dest="cmd")
    dp = sub.add_parser("dump", help="save every read-only endpoint, logs, journal (default)")
    dp.add_argument("--no-coredump", action="store_true", help="do not download the core dump even if present")
    wp = sub.add_parser("watch", help="compact live status line")
    wp.add_argument("--interval", type=float, default=1.0, help="seconds between polls (default 1)")
    wp.add_argument("--logs", action="store_true", help="also tail the device log ring")
    gp = sub.add_parser("get", help="GET one API path")
    gp.add_argument("path", help="e.g. /api/gnss")
    args = ap.parse_args()
    args.cmd = args.cmd or "dump"
    if args.cmd == "dump":
        args.no_coredump = getattr(args, "no_coredump", False)

    run_dir = make_run_dir(args.log_dir, "station", args.label or args.cmd)
    write_command_line(run_dir)
    st = Station(args.host, None, timeout=args.timeout)
    stop = StopFlag()
    try:
        try:
            session = st.get("/api/session", relogin=False)
        except (httpx.HTTPError, ApiError) as e:
            print(f"cannot reach http://{args.host}/api/session: {e}")
            return 2
        (run_dir / "session-before-login.json").write_text(json.dumps(session, indent=2) + "\n", encoding="utf-8")
        if not session.get("admin_set", True):
            print("no admin password is set yet: finish first setup on the station's AP (claim), then retry")
            return 2
        try:
            st.login()
        except ApiError as e:
            print(f"login failed: {e} {json.dumps(e.body)}")
            return 2
        print(f"logged in to {args.host}; log: {run_dir}")
        if args.cmd == "dump":
            return dump(st, run_dir, args)
        if args.cmd == "watch":
            return watch(st, run_dir, args, stop)
        data = st.get(args.path if args.path.startswith("/") else "/" + args.path)
        text = json.dumps(data, indent=2) if not isinstance(data, bytes) else f"<{len(data)} bytes>"
        print(text)
        (run_dir / "get.json").write_text(text + "\n", encoding="utf-8")
        return 0
    finally:
        st.close()


if __name__ == "__main__":
    sys.exit(main())
