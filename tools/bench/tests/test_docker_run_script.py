# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""docker/bkg-caster/run.sh without Docker: --print shows the docker command
it would run, so where the caster is published (127.0.0.1 by default, every
interface with --lan) is checked on any machine."""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

RUN_SH = Path(__file__).resolve().parents[1] / "docker" / "bkg-caster" / "run.sh"
IMAGE = "onocoydiy-bench-bkg-caster"
LAN_WARNING = "reachable from your LAN: use the throwaway test passwords only"


def run(*args: str) -> subprocess.CompletedProcess:
    # No docker on the PATH: --print must not need it.
    env = {k: v for k, v in os.environ.items() if k != "BKG_CASTER_IMAGE"}
    env["PATH"] = "/usr/bin:/bin"
    return subprocess.run([shutil.which("bash") or "/bin/bash", str(RUN_SH), *args], capture_output=True,
                          text=True, timeout=30, env=env)


def test_default_publishes_on_localhost_only():
    r = run("--print")
    assert r.returncode == 0, r.stderr
    assert r.stdout == f"docker run --rm --name onocoydiy-bench-bkg-caster -p 127.0.0.1:2101:2101 {IMAGE}\n"
    assert " -p 127.0.0.1:2101:2101 " in r.stdout and "0.0.0.0" not in r.stdout
    assert LAN_WARNING not in r.stderr


def test_lan_publishes_on_every_interface_and_warns():
    r = run("--lan", "--print")
    assert r.returncode == 0, r.stderr
    assert " -p 0.0.0.0:2101:2101 " in r.stdout and "127.0.0.1" not in r.stdout
    assert LAN_WARNING in r.stderr


def test_port_and_detach_reach_the_command():
    r = run("--print", "--lan", "--detach", "--port", "2102", "--name", "bench-a")
    assert r.returncode == 0, r.stderr
    assert r.stdout == f"docker run -d --rm --name bench-a -p 0.0.0.0:2102:2101 {IMAGE}\n"


def test_stop_prints_the_removal_without_a_warning():
    r = run("--print", "--stop", "--lan")
    assert r.returncode == 0, r.stderr
    assert r.stdout == "docker rm -f onocoydiy-bench-bkg-caster\n"
    assert LAN_WARNING not in r.stderr


@pytest.mark.parametrize("port", ["0", "65536", "x"])
def test_bad_port_is_refused(port):
    r = run("--print", "--port", port)
    assert r.returncode == 2 and r.stdout == ""
