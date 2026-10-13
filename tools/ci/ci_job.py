#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Run one job of .github/workflows/build.yml the way its runner does, inside the job's own container.

    python tools/ci/ci_job.py WORKFLOW JOB --log FILE [--artifacts DIR]

Not for direct use: tools/ci/host_tests_linux.sh and tools/ci/qemu_linux.sh start the image the job
pins, with a copy of the working tree in /work, and run this there with the image's ESP-IDF Python
(PyYAML), so the steps come from build.yml itself and never drift from CI.

Each step runs as a job step runs: a `run:` step in its working directory, with the job's and the
step's `env:`, under `bash --noprofile --norc -eo pipefail` (`shell: bash`; `bash -e` when no shell
is given), starting from the image's environment (the ESP-IDF environment only where the step sources
export.sh itself). The actions the jobs use stand in as follows:

  actions/checkout           the copy of the working tree (nothing to do)
  astral-sh/setup-uv         uv of the pinned `version`, from its GitHub release, checked against the
                             release's SHA-256 file and put on PATH
  actions/upload-artifact    every `path` copied into --artifacts (nothing without --artifacts)

Any other action, a step condition other than success() or always(), or an expression in `env:`
stops the run before the first step: this script would not run that job as CI does. As on a runner,
a failed step skips the rest except steps with `if: always()`.

Every step's output goes to stdout and to --log. Exit status 0 when every step passed.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

import yaml

WORKSPACE = "/work"
UV_URL = "https://github.com/astral-sh/uv/releases/download/{v}/uv-{arch}-unknown-linux-gnu.tar.gz"
UV_ARCH = {"x86_64": "x86_64", "aarch64": "aarch64", "arm64": "aarch64"}


class JobError(Exception):
    """build.yml asks for something this script cannot do as CI does."""


def condition(step: dict) -> str:
    """'success' (the default) or 'always'; anything else is refused."""
    cond = step.get("if")
    if cond is None:
        return "success"
    text = str(cond).strip()
    if text.startswith("${{") and text.endswith("}}"):
        text = text[3:-2].strip()
    if text in ("success()", "always()"):
        return text[:-2]
    raise JobError(f"step '{step_name(step)}' has the condition {cond!r} (only success() and always() are run)")


def step_name(step: dict) -> str:
    return str(step.get("name") or step.get("uses") or "run")


def step_env(job: dict, step: dict) -> dict[str, str]:
    env = {**(job.get("env") or {}), **(step.get("env") or {})}
    for k, v in env.items():
        if "${{" in str(v):
            raise JobError(f"step '{step_name(step)}' sets {k} from an expression, which this script cannot evaluate")
    return {k: str(v) for k, v in env.items()}


def plan(job: dict) -> list[tuple[str, dict]]:
    """Each step as (kind, step), checked before anything runs."""
    if "defaults" in job:
        raise JobError("the job has defaults:, which this script does not apply")
    if "services" in job:
        raise JobError("the job has services:, which this script does not start")
    out = []
    for st in job.get("steps") or []:
        condition(st)
        step_env(job, st)
        if "run" in st:
            shell = st.get("shell", "")
            if shell not in ("", "bash"):
                raise JobError(f"step '{step_name(st)}' uses shell {shell}, not bash")
            out.append(("run", st))
            continue
        uses = str(st.get("uses", ""))
        action = uses.split("@", 1)[0]
        if action == "actions/checkout":
            out.append(("checkout", st))
        elif action == "astral-sh/setup-uv":
            version = str((st.get("with") or {}).get("version", ""))
            if not re.fullmatch(r"\d+\.\d+\.\d+", version):
                raise JobError(f"step '{step_name(st)}': setup-uv without a pinned version (found {version!r})")
            out.append(("setup-uv", st))
        elif action == "actions/upload-artifact":
            out.append(("upload", st))
        else:
            raise JobError(f"step '{step_name(st)}' uses {uses}, which this script does not stand in for")
    if not any(kind == "run" for kind, _ in out):
        raise JobError("the job has no run steps")
    return out


class Runner:
    def __init__(self, job: dict, log: Path, artifacts: Path | None):
        self.job = job
        self.log = open(log, "a", encoding="utf-8", errors="replace")
        self.artifacts = artifacts
        self.tools = Path(tempfile.mkdtemp(prefix="ci-tools-"))
        self.temp = Path(tempfile.mkdtemp(prefix="ci-runner-temp-"))
        self.path_extra: list[str] = []

    def say(self, text: str) -> None:
        print(text, flush=True)
        self.log.write(text + "\n")
        self.log.flush()

    def env(self, step: dict) -> dict[str, str]:
        env = dict(os.environ)
        env.update(step_env(self.job, step))
        env.update({"CI": "true", "GITHUB_WORKSPACE": WORKSPACE, "RUNNER_TEMP": str(self.temp)})
        if self.path_extra:
            env["PATH"] = os.pathsep.join(self.path_extra + [env.get("PATH", "")])
        return env

    def run_step(self, step: dict) -> bool:
        wd = Path(WORKSPACE) / str(step.get("working-directory", "."))
        argv = ["bash", "--noprofile", "--norc", "-eo", "pipefail"] if step.get("shell") == "bash" else ["bash", "-e"]
        with tempfile.NamedTemporaryFile("w", suffix=".sh", dir=self.temp, delete=False) as f:
            f.write(str(step["run"]))
            script = f.name
        proc = subprocess.Popen(argv + [script], cwd=wd, env=self.env(step), stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        assert proc.stdout is not None
        for raw in proc.stdout:
            line = raw.decode("utf-8", errors="replace")
            sys.stdout.write(line)
            sys.stdout.flush()
            self.log.write(line)
        self.log.flush()
        return proc.wait() == 0

    def setup_uv(self, step: dict) -> bool:
        version = str(step["with"]["version"])
        arch = UV_ARCH.get(platform.machine())
        if arch is None:
            self.say(f"setup-uv: no uv build for {platform.machine()}")
            return False
        url = UV_URL.format(v=version, arch=arch)
        try:
            data = urllib.request.urlopen(url, timeout=120).read()
            want = urllib.request.urlopen(url + ".sha256", timeout=120).read().decode().split()[0].lower()
        except OSError as e:
            self.say(f"setup-uv: {url}: {e}")
            return False
        got = hashlib.sha256(data).hexdigest()
        if got != want:
            self.say(f"setup-uv: {url}: SHA-256 {got}, the release says {want}")
            return False
        tarball = self.temp / "uv.tar.gz"
        tarball.write_bytes(data)
        with tarfile.open(tarball) as t:
            for m in t.getmembers():
                name = Path(m.name).name
                if m.isfile() and name in ("uv", "uvx"):
                    src = t.extractfile(m)
                    assert src is not None
                    dst = self.tools / name
                    dst.write_bytes(src.read())
                    dst.chmod(0o755)
        if not (self.tools / "uv").exists():
            self.say(f"setup-uv: no uv binary in {url}")
            return False
        self.path_extra.insert(0, str(self.tools))
        out = subprocess.run([str(self.tools / "uv"), "--version"], capture_output=True, text=True)
        self.say(f"setup-uv: {out.stdout.strip() or out.stderr.strip()} ({url}, SHA-256 checked)")
        return out.returncode == 0

    def upload(self, step: dict) -> bool:
        if self.artifacts is None:
            self.say("upload-artifact: not kept (no --artifacts directory)")
            return True
        paths = [p.strip() for p in str((step.get("with") or {}).get("path", "")).splitlines() if p.strip()]
        for p in paths:
            src = Path(WORKSPACE) / p
            dst = self.artifacts / p
            if src.is_dir():
                shutil.copytree(src, dst, dirs_exist_ok=True)
            elif src.is_file():
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)
            else:
                self.say(f"upload-artifact: {p}: not there")
                continue
            self.say(f"upload-artifact: {p}")
        return True

    def run(self, steps: list[tuple[str, dict]]) -> int:
        failed = False
        for kind, st in steps:
            name = step_name(st)
            if kind == "checkout":
                self.say(f"=== step: {name} (the copy of the working tree stands in)")
                continue
            if failed and condition(st) != "always":
                self.say(f"=== step: {name}: skipped (an earlier step failed)")
                continue
            where = f" (in {st.get('working-directory', '.')})" if kind == "run" else ""
            self.say(f"=== step: {name}{where}")
            if kind == "run":
                ok = self.run_step(st)
            elif kind == "setup-uv":
                ok = self.setup_uv(st)
            else:
                ok = self.upload(st)
            if not ok:
                self.say(f"=== step: {name}: FAILED")
                failed = True
        return 1 if failed else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("workflow")
    ap.add_argument("job")
    ap.add_argument("--log", required=True, help="every step's output is appended here too")
    ap.add_argument("--artifacts", help="where upload-artifact steps copy their paths")
    ap.add_argument("--list", action="store_true", help="print the job's steps and stop")
    args = ap.parse_args()
    jobs = yaml.safe_load(open(args.workflow, encoding="utf-8")).get("jobs") or {}
    if args.job not in jobs:
        print(f"ci_job.py: no job {args.job} in {args.workflow}", file=sys.stderr)
        return 2
    try:
        steps = plan(jobs[args.job])
    except JobError as e:
        print(f"ci_job.py: {args.workflow}, job {args.job}: {e}", file=sys.stderr)
        return 2
    if args.list:
        for kind, st in steps:
            print(f"{kind}: {step_name(st)}")
        return 0
    artifacts = Path(args.artifacts) if args.artifacts else None
    if artifacts is not None:
        artifacts.mkdir(parents=True, exist_ok=True)
    return Runner(jobs[args.job], Path(args.log), artifacts).run(steps)


if __name__ == "__main__":
    sys.exit(main())
