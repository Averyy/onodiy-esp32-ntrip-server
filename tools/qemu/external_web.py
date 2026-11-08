#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Boot the external-web Ethernet image and check its real HTTP/WS contract.

Build first: tools/ci/external_web.sh /tmp/onodiy-external-web --qemu
Run: uv run --no-project python tools/qemu/external_web.py \
    --build-dir /tmp/onodiy-external-web/build-16MB-qemu --out /tmp/external-web-smoke
Fresh flash/NVS on every run. Standard library only; QEMU always terminates.
"""
from __future__ import annotations

import argparse
import base64
import gzip
import hashlib
import http.client
import json
import os
from pathlib import Path
import secrets
import shutil
import signal
import socket
import struct
import subprocess
import time

FATAL = ("Guru Meditation", "abort()", "stack overflow", "assert failed:", "Backtrace:", "task_wdt:")


def prepare_flash(build, out):
    config = json.loads((build / "config/sdkconfig.json").read_text())
    assert config.get("NETWORK_QEMU_ETHERNET") and config.get("ETH_USE_OPENETH"), "requires QEMU build"
    python = Path(os.environ["IDF_PYTHON_ENV_PATH"]) / "bin/python"
    flash = out / "flash.bin"
    subprocess.run(["uv", "run", "--no-project", str(python), "-m", "esptool", "--chip=esp32s3", "merge-bin",
                    f"--output={flash}", "--pad-to-size=16MB", "@flash_args"], cwd=build, check=True)
    efuse = out / "efuse.bin"
    subprocess.run(["uv", "run", "--no-project", str(python), "-c",
                    "import os,sys; sys.path.insert(0,os.path.join(os.environ['IDF_PATH'],'tools')); "
                    "from idf_py_actions.qemu_ext import QEMU_TARGETS; "
                    "open(sys.argv[1],'wb').write(QEMU_TARGETS['esp32s3'].default_efuse)", str(efuse)], check=True)
    return flash, efuse


class WebSocket:
    def __init__(self, port, cookie=None, expected=101):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=8)
        self.sock.settimeout(8)
        self.file = self.sock.makefile("rb")
        key = base64.b64encode(secrets.token_bytes(16)).decode()
        host = f"127.0.0.1:{port}"
        request = (f"GET /ws HTTP/1.1\r\nHost: {host}\r\nOrigin: http://{host}\r\n"
                   f"Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                   "Sec-WebSocket-Version: 13\r\n" + (f"Cookie: {cookie}\r\n" if cookie else "") + "\r\n")
        try:
            self.sock.sendall(request.encode())
            line = self.file.readline()
            assert int(line.split()[1]) == expected, line
            headers = {}
            while (line := self.file.readline()) not in (b"\r\n", b""):
                name, value = line.decode().split(":", 1)
                headers[name.lower()] = value.strip()
            if expected == 101:
                accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
                assert headers["sec-websocket-accept"] == accept
        except BaseException:
            self.close()
            raise

    def exact(self, count):
        data = self.file.read(count)
        assert len(data) == count, "WS closed before complete frame"
        return data

    def message(self):
        first, second = self.exact(2)
        assert first & 0x80 and not second & 0x80, "expected unmasked complete server frame"
        size = second & 0x7f
        if size == 126: size = struct.unpack("!H", self.exact(2))[0]
        elif size == 127: size = struct.unpack("!Q", self.exact(8))[0]
        assert size <= 16384
        data = self.exact(size)
        assert first & 0x0f == 1, f"unexpected WS opcode {first & 0x0f}"
        return json.loads(data)

    def until(self, predicate):
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            message = self.message()
            if predicate(message): return message
        raise AssertionError("WS message deadline")

    def close(self):
        self.file.close()
        self.sock.close()

    def __enter__(self): return self
    def __exit__(self, *args): self.close()


def smoke(port, out):
    transcript = []

    def request(method, path, body=None, cookie=None, headers=None, expected=200):
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=8)
        fields = {"Accept-Encoding": "gzip"}
        if body is not None:
            body = json.dumps(body)
            fields["Content-Type"] = "application/json"
        if cookie: fields["Cookie"] = cookie
        fields.update(headers or {})
        try:
            connection.request(method, path, body, fields)
            response = connection.getresponse()
            data = response.read()
            head = dict((k.lower(), v) for k, v in response.getheaders())
            transcript.append({"method": method, "path": path, "status": response.status})
            assert response.status == expected, (method, path, response.status, data)
            assert head.get("x-content-type-options") == "nosniff"
            return head, data
        finally:
            connection.close()
            (out / "requests.json").write_text(json.dumps(transcript, indent=2) + "\n")

    def api(method, path, body=None, cookie=None, **kwargs):
        headers, data = request(method, path, body, cookie, **kwargs)
        return headers, json.loads(data)

    deadline = time.monotonic() + 120
    while True:
        try:
            api("GET", "/api/session")
            break
        except (OSError, http.client.HTTPException):
            if time.monotonic() > deadline: raise
            time.sleep(0.5)
    for path in ("/", "/app.js", "/app.css", "/favicon.svg"):
        head, data = request("GET", path)
        assert head.get("content-encoding") == "gzip"
        raw = gzip.decompress(data)
        assert raw
        if path == "/":
            csp = head["content-security-policy"]
            assert "script-src 'self'" in csp and "unsafe-inline" not in csp and "unsafe-eval" not in csp
            assert b"<title>Counter</title>" in raw and b"maplibre" not in raw
        if path == "/app.js":
            assert b"station" not in raw.lower() and b"counter." in raw
    api("GET", "/api/status", expected=403)
    api("GET", "/api/product/settings", expected=401)
    with WebSocket(port, expected=403): pass
    password = secrets.token_urlsafe(18)
    api("POST", "/api/setup/admin", {"password": password}, headers={"Content-Type": "text/plain"}, expected=415)
    api("POST", "/api/setup/admin", {"password": password}, headers={"Origin": "http://elsewhere.invalid"}, expected=403)
    head, claimed = api("POST", "/api/setup/admin", {"password": password})
    assert claimed["authenticated"] and claimed["persisted"]
    cookie = head["set-cookie"].split(";", 1)[0]
    assert "httponly" in head["set-cookie"].lower() and "samesite=strict" in head["set-cookie"].lower()
    _, status = api("GET", "/api/status", cookie=cookie)
    deadline = time.monotonic() + 30
    while status["network"]["online"] is not True and time.monotonic() < deadline:
        time.sleep(0.5)
        _, status = api("GET", "/api/status", cookie=cookie)
    assert not status["public"] and status["api"] == 1
    assert set(status) == {"api", "public", "uptime_s", "product", "network", "preferences"}
    assert status["network"]["online"] is True and isinstance(status["product"]["counter"], int)
    assert status["preferences"]["settings_available"] is True
    for path in ("/api/casters", "/api/receiver", "/api/health", "/api/diag", "/api/system", "/api/update", "/api/wifi", "/api/setup/step"):
        api("GET", path, cookie=cookie, expected=404)
    api("PUT", "/api/product/settings", {"public_status": True}, cookie,
        headers={"Content-Type": "text/plain"}, expected=415)
    api("PUT", "/api/product/settings", {"public_status": True}, cookie,
        headers={"Origin": "http://elsewhere.invalid"}, expected=403)
    api("PUT", "/api/product/settings", {"setup_step": "x" * 300}, cookie, expected=413)
    with WebSocket(port, cookie) as ws:
        hello = ws.until(lambda m: m.get("type") == "hello")
        assert hello["admin"] is True
        first = ws.until(lambda m: m.get("type") == "status")
        assert first["full"] and "preferences" in first["data"]
        later = ws.until(lambda m: m.get("type") == "status" and m.get("data", {}).get("product", {}).get("counter", -1) > first["data"]["product"]["counter"])
        assert later["uptime_s"] >= first["uptime_s"]
        # The shared dispatcher attributes changes to this browser tab.
        api("PUT", "/api/product/settings", {"public_status": True, "setup_step": "visibility"}, cookie,
            headers={"X-Client-ID": "external-smoke"})
        event = ws.until(lambda m: m.get("event") == "settings_changed")
        assert event["ns"] == "counter_ui" and event["client"] == "external-smoke"
    _, public = api("GET", "/api/status")
    assert public["public"] and set(public) == {"api", "public", "uptime_s", "product", "network"}
    assert set(public["network"]) == {"online"}
    with WebSocket(port) as ws:
        assert ws.until(lambda m: m.get("type") == "hello")["admin"] is False
        first = ws.until(lambda m: m.get("type") == "status")
        assert first["full"] and "preferences" not in first["data"] and set(first["data"]["network"]) == {"online"}
    api("PUT", "/api/product/settings", {"setup_done": True}, cookie)
    _, session = api("GET", "/api/session", cookie=cookie)
    assert session["setup_done"]
    api("DELETE", "/api/session", cookie=cookie)
    api("GET", "/api/product/settings", cookie=cookie, expected=401)
    head, _ = api("POST", "/api/session", {"password": password, "remember": True})
    cookie = head["set-cookie"].split(";", 1)[0]
    api("PUT", "/api/product/settings", {"public_status": False}, cookie)
    api("GET", "/api/status", expected=403)
    api("DELETE", "/api/session", cookie=cookie)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    out = args.out.resolve()
    qemu = shutil.which("qemu-system-xtensa")
    if not qemu: parser.error("qemu-system-xtensa is not on PATH")
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    for signum in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, interrupted)
    flash, efuse = prepare_flash(args.build_dir.resolve(), out)
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    with (out / "qemu.log").open("wb") as log:
        process = subprocess.Popen([qemu, "-M", "esp32s3", "-m", "8M", "-accel", "tcg,thread=single",
            "-drive", f"file={flash},if=mtd,format=raw", "-drive", f"file={efuse},if=none,format=raw,id=efuse",
            "-global", "driver=nvram.esp32s3.efuse,property=drive,value=efuse",
            "-nic", f"user,model=open_eth,restrict=on,hostfwd=tcp:127.0.0.1:{port}-:80",
            "-nographic", "-monitor", "none", "-serial", "stdio"], stdout=log, stderr=subprocess.STDOUT)
        try:
            smoke(port, out)
            assert process.poll() is None, "QEMU exited"
        finally:
            process.terminate()
            try: process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=10)
    text = (out / "qemu.log").read_text(errors="replace")
    assert not any(marker in text for marker in FATAL), "fatal firmware log"
    (out / "summary.txt").write_text("PASS: gzip/CSP, claim/login/logout, CSRF/limits, visibility, truthful product status, WS and absent station routes\n")
    print((out / "summary.txt").read_text(), end="")


if __name__ == "__main__":
    main()
