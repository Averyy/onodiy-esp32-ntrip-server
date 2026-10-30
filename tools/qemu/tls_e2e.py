#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyopenssl==26.4.0"]
# ///
"""Run the existing recorded-stream QEMU scenarios through verified TLS 1.2.

Creates a temporary local test certificate, uploads it through the emulator's
custom-CA API, and requests server-initiated renegotiation on each connection.
No physical station or external caster is accessed. Build QEMU first, then:
  uv run tools/qemu/tls_e2e.py --out build-qemu/tls-stream
  uv run tools/qemu/tls_e2e.py --scenario local-caster --out build-qemu/tls-rovers
"""
import argparse
import os
import select
import socket
import subprocess
import time
from pathlib import Path

from OpenSSL import SSL

import e2e


class TLSSocket:
    """Socket interface for the existing caster, with real OpenSSL readiness."""
    def __init__(self, sock, context, report):
        self.sock = sock
        sock.setblocking(False)
        self.tls = SSL.Connection(context, sock)
        self.tls.set_accept_state()
        self.timeout = 15.0
        self.report = report
        self.requested = self.completed = False
        self.call(self.tls.do_handshake)
        self.started = time.monotonic()

    def settimeout(self, seconds):
        self.timeout = seconds

    def call(self, fn, *args):
        end = time.monotonic() + self.timeout
        while True:
            try:
                return fn(*args)
            except (SSL.WantReadError, SSL.WantWriteError) as exc:
                left = end - time.monotonic()
                if left <= 0:
                    raise socket.timeout() from exc
                read = isinstance(exc, SSL.WantReadError)
                ready = select.select([self.sock] if read else [], [] if read else [self.sock], [], left)
                if not ready[0] and not ready[1]:
                    raise socket.timeout() from exc
            except SSL.ZeroReturnError:
                return b""
            except SSL.Error as exc:
                raise OSError(str(exc)) from exc

    def recv(self, size):
        if not self.requested and time.monotonic() - self.started >= 15:
            self.requested = self.tls.renegotiate()
            if not self.requested:
                raise OSError("server could not request TLS renegotiation")
            self.report("requested")
            self.call(self.tls.do_handshake)
        data = self.call(self.tls.recv, size)
        if (data and self.requested and not self.completed and
                not self.tls.renegotiate_pending() and self.tls.total_renegotiations() > 0):
            self.completed = True
            self.report("completed")
        return data

    def sendall(self, data):
        while data:
            sent = self.call(self.tls.send, data)
            if not sent:
                raise OSError("TLS write made no progress")
            data = data[sent:]

    def shutdown(self, how):
        self.sock.shutdown(how)

    def close(self):
        self.sock.close()


class TLSCaster(e2e.FakeCaster):
    def __init__(self, *args, context, **kwargs):
        self.context = context
        self.renegotiations = []
        self.tls_errors = []
        super().__init__(*args, **kwargs)

    def _handle(self, sock, conn):
        def report(phase):
            self.renegotiations.append((conn.n, phase))
            self._log(f"TLS connection {conn.n}: renegotiation {phase}")
        try:
            wrapped = TLSSocket(sock, self.context, report)
        except OSError as exc:
            self.tls_errors.append(str(exc))
            sock.close()
            return
        super()._handle(wrapped, conn)


class TLSE2E(e2e.E2E):
    def __init__(self, args, schedule):
        super().__init__(args, schedule)
        key, cert = self.out / "tls-key.pem", self.out / "tls-cert.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
                        "-subj", "/CN=10.0.2.2", "-addext", "subjectAltName=IP:10.0.2.2,IP:127.0.0.1",
                        "-keyout", str(key), "-out", str(cert)], check=True, capture_output=True)
        key.chmod(0o600)
        self.pem = cert.read_text()
        self.context = SSL.Context(SSL.TLS_SERVER_METHOD)
        self.context.set_min_proto_version(SSL.TLS1_2_VERSION)
        self.context.set_max_proto_version(SSL.TLS1_2_VERSION)
        self.context.use_privatekey_file(str(key))
        self.context.use_certificate_file(str(cert))
        self.ca_saved = False

    def stream_caster(self):
        return TLSCaster(0, sequence=[e2e.Entry(self.stream_row())], mount=e2e.STREAM_MOUNT,
                         password=e2e.STREAM_PASSWORD, record=True, run_dir=self.out / "caster",
                         say=self.say, context=self.context).start()

    def stream_body(self, port, password):
        return {**super().stream_body(port, password), "tls": True, "trust": "custom_ca"}

    def stream(self):
        super().stream()
        # The scenario ends right after its restart, while slot 1 reconnects: stopping then would cut that TLS
        # handshake, which the caster counts as a failure. Wait for the reconnect, and check it.
        def streaming():
            s = self.slot(1)["status"]
            return s if s.get("state") == "streaming" else None
        self.check(self.poll(streaming, 60) is not None, "slot 1 streams over TLS again after the restart")

    def api(self, method, path, body=None, timeout=30):
        if method == "PUT" and path == "/api/casters/1" and body and body.get("tls") and not self.ca_saved:
            status, data = super().api("PUT", "/api/casters/1/ca", {"pem": self.pem}, timeout)
            if not self.check(status == 200, f"emulator trusts only the local test CA (HTTP {status})"):
                raise RuntimeError("test CA was rejected")
            self.ca_saved = True
        return super().api(method, path, body, timeout)

    def console_checks(self):
        super().console_checks()
        caster = self.caster
        completed = [n for n, phase in caster.renegotiations if phase == "completed"] if caster else []
        self.check(bool(completed), f"TLS 1.2 server renegotiation completed on connections {completed}")
        self.check(caster is not None and not caster.tls_errors, "no TLS handshake failures")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default="build-qemu/tls-e2e")
    parser.add_argument("--scenario", choices=("stream", "local-caster"), default="stream")
    args = parser.parse_args()
    if not os.environ.get("IDF_PATH"):
        parser.error("source the ESP-IDF environment first")
    args.caster, args.ntrip_version = "fake", 2
    args.long, args.observe, args.timeout = False, 60, 150
    return TLSE2E(args, e2e.load(e2e.DEFAULT_FIXTURE)).run()


if __name__ == "__main__":
    raise SystemExit(main())
