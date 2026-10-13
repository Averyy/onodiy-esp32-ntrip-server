#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Run one job of .github/workflows/build.yml the way its runner does, inside the job's own container.

    python tools/ci/ci_job.py WORKFLOW JOB --log FILE [--artifacts DIR]

Not for direct use: tools/ci/host_tests_linux.sh and tools/ci/qemu_linux.sh start the image the job
pins, with a copy of the working tree in /work, and run this there with the image's ESP-IDF Python
(PyYAML), so the steps come from build.yml itself and never drift from CI.

build.yml is read as Actions reads a workflow: a plain scalar is typed by the YAML 1.2 core schema
(`on`, `yes` and `1:30` stay text, `010` is ten), and a value given where Actions wants text (an
`env:` value) becomes text as Actions writes it: true and false in lower case, a null as nothing, a
number as .NET prints a double with 15 digits (1.0 as "1", 1e20 as "1E+20").

Each step runs as a job step runs: a `run:` step in its working directory, with the job's and the
step's `env:`, under `bash --noprofile --norc -eo pipefail` (`shell: bash`; `bash -e` when no shell
is given), starting from the image's environment (the ESP-IDF environment only where the step sources
export.sh itself). The actions the jobs use stand in as follows:

  actions/checkout           the copy of the working tree (nothing to do; input fetch-depth)
  astral-sh/setup-uv         uv of the pinned `version`, from its GitHub release, checked against the
                             release's SHA-256 file and put on PATH (input version only)
  actions/upload-artifact    every `path` copied into --artifacts (nothing without --artifacts),
                             hidden files left out; with `if-no-files-found: error` the step fails
                             when no file is found, as the action does (inputs name, path,
                             if-no-files-found, retention-days)

As on a runner, a failed step skips the rest except steps with `if: always()`, and a step with
`continue-on-error: true` fails without failing the job. A `run:` step past its `timeout-minutes`
fails (whole minutes: the runner drops the fraction, so less than 1 sets no limit); it is stopped
with the runner's sequence, SIGINT, then SIGTERM 7.5 s later and SIGKILL 2.5 s after that, sent here
to the step's process group.

Anything else stops the run before the first step, since this script would not run that job as CI
does: a workflow-level `env:` or `defaults:`, a job key other than name, runs-on, container (an image
name), steps, env, needs, permissions, concurrency, outputs and if (so no matrix, services, defaults,
job timeout or job continue-on-error), a step key a step of its kind cannot have here, a condition
other than success() or always(), an expression in a value the script uses (env, run, shell,
working-directory, an input a stand-in reads), any other action or input, an upload path that is a
pattern or leaves the workspace, `timeout-minutes` on an action, and a continue-on-error or
timeout-minutes that is not written out as a literal.

Every step's output goes to stdout and to --log. Exit status 0 when every step passed.
"""

from __future__ import annotations

import argparse
import codecs
import hashlib
import math
import os
import platform
import re
import selectors
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
from pathlib import Path
from typing import Callable

import yaml

WORKSPACE = "/work"
UV_URL = "https://github.com/astral-sh/uv/releases/download/{v}/uv-{arch}-unknown-linux-gnu.tar.gz"
UV_ARCH = {"x86_64": "x86_64", "aarch64": "aarch64", "arm64": "aarch64"}

# What a workflow, a job and a step may set for this script to run the job as CI does.
WORKFLOW_KEYS = frozenset({"name", "run-name", "on", "permissions", "concurrency", "jobs"})
JOB_KEYS = frozenset({"name", "runs-on", "container", "steps", "env", "needs", "permissions", "concurrency",
                      "outputs", "if"})
RUN_STEP_KEYS = frozenset({"name", "id", "if", "run", "shell", "working-directory", "env", "continue-on-error",
                           "timeout-minutes"})
ACTION_STEP_KEYS = frozenset({"name", "id", "if", "uses", "with", "env", "continue-on-error"})
# The actions that stand in, each with the inputs it takes.
ACTION_INPUTS = {
    "actions/checkout": frozenset({"fetch-depth"}),
    "astral-sh/setup-uv": frozenset({"version"}),
    "actions/upload-artifact": frozenset({"name", "path", "if-no-files-found", "retention-days"}),
}
INT32_MAX = 2**31 - 1


class JobError(Exception):
    """build.yml asks for something this script cannot do as CI does."""


# --- build.yml as Actions reads it ---------------------------------------------------------------


class ActionsLoader(yaml.SafeLoader):
    """YAML with plain scalars typed by the YAML 1.2 core schema only, as Actions reads a workflow.

    PyYAML's own rules are YAML 1.1's: they read `on`, `yes` and `off` as booleans, `010` as eight,
    `1:30` as ninety and `2026-10-13` as a date. A number here is a float, as Actions keeps every
    number as a double; quoted and block scalars are always text."""

    yaml_implicit_resolvers: dict = {}


_NULL = re.compile(r"(?:|~|null|Null|NULL)\Z")
_BOOL = re.compile(r"(?:true|True|TRUE|false|False|FALSE)\Z")
_INT = re.compile(r"(?:[-+]?[0-9]+|0x[0-9a-fA-F]+|0o[0-7]+)\Z")
_FLOAT = re.compile(r"(?:[-+]?(?:\.[0-9]+|[0-9]+(?:\.[0-9]*)?)(?:[eE][-+]?[0-9]+)?"
                    r"|[-+]?\.(?:inf|Inf|INF)|\.(?:nan|NaN|NAN))\Z")


def _scalar_error(node: yaml.Node, text: str, what: str) -> JobError:
    return JobError(f"build.yml line {node.start_mark.line + 1}: {text!r} {what}")


def _construct_null(loader: yaml.Loader, node: yaml.Node) -> None:
    text = loader.construct_scalar(node)
    if not _NULL.match(text):
        raise _scalar_error(node, text, "is not a null")
    return None


def _construct_bool(loader: yaml.Loader, node: yaml.Node) -> bool:
    text = loader.construct_scalar(node)
    if not _BOOL.match(text):
        raise _scalar_error(node, text, "is not a boolean (true or false)")
    return text in ("true", "True", "TRUE")


def _construct_int(loader: yaml.Loader, node: yaml.Node) -> float:
    text = loader.construct_scalar(node)
    if not _INT.match(text):
        raise _scalar_error(node, text, "is not an integer")
    if text[:2] in ("0x", "0o"):
        # Actions reads these as 32-bit integers; one past INT32_MAX is not certain to read as written.
        value = int(text[2:], 16 if text[1] == "x" else 8)
        if value > INT32_MAX:
            raise _scalar_error(node, text, "is beyond a 32-bit integer")
        return float(value)
    value = float(text)
    if math.isinf(value):
        raise _scalar_error(node, text, "is beyond a double")
    return value


def _construct_float(loader: yaml.Loader, node: yaml.Node) -> float:
    text = loader.construct_scalar(node)
    if not _FLOAT.match(text):
        raise _scalar_error(node, text, "is not a number")
    if text.lstrip("+-").lower() in (".inf", ".nan"):
        return float(text.replace(".", "", 1))
    value = float(text)
    if math.isinf(value):
        raise _scalar_error(node, text, "is beyond a double")
    return value


for _tag, _regexp, _construct in (
        ("tag:yaml.org,2002:null", _NULL, _construct_null),
        ("tag:yaml.org,2002:bool", _BOOL, _construct_bool),
        ("tag:yaml.org,2002:int", _INT, _construct_int),
        ("tag:yaml.org,2002:float", _FLOAT, _construct_float)):
    ActionsLoader.add_implicit_resolver(_tag, _regexp, None)
    ActionsLoader.add_constructor(_tag, _construct)


def load_workflow(text: str) -> dict:
    data = yaml.load(text, Loader=ActionsLoader)
    if not isinstance(data, dict):
        raise JobError("the workflow is not a mapping")
    return data


def number_text(value: float) -> str:
    """A number as Actions makes text of it: .NET's Double.ToString("G15") in the invariant culture."""
    if math.isnan(value):
        return "NaN"
    if math.isinf(value):
        return "Infinity" if value > 0 else "-Infinity"
    # The same rule as .NET's G15: 15 significant digits, trailing zeros dropped, scientific notation
    # for an exponent below -4 or from 15 up, with at least two exponent digits.
    return format(value, ".15g").replace("e", "E")


def actions_text(value: object, what: str) -> str:
    """A scalar where Actions wants text, as Actions converts it."""
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return number_text(float(value))
    if isinstance(value, str):
        return value
    raise JobError(f"{what} is not a single value")


# --- what the job asks for, checked before anything runs -------------------------------------------


def _only(mapping: dict, allowed: frozenset, what: str) -> None:
    extra = sorted(str(k) for k in mapping if k not in allowed)
    if extra:
        raise JobError(f"{what} sets {', '.join(extra)}, which this script does not reproduce")


def _literal(value: object, what: str) -> str:
    """Text the script uses as it stands: refused if Actions would evaluate an expression in it."""
    text = actions_text(value, what)
    if "${{" in text:
        raise JobError(f"{what} has an expression, which this script cannot evaluate")
    return text


def _condition(value: object, what: str) -> str:
    if value is None:
        return "success"
    text = actions_text(value, f"{what}'s if:").strip()
    if text.startswith("${{") and text.endswith("}}"):
        text = text[3:-2].strip()
    if text in ("success()", "always()"):
        return text[:-2]
    raise JobError(f"{what} has the condition {value!r} (only success() and always() are run)")


def step_name(step: dict) -> str:
    for key in ("name", "uses"):
        if step.get(key) not in (None, ""):
            return actions_text(step[key], "a step's name")
    return "run"


def condition(step: dict) -> str:
    """'success' (the default) or 'always'; anything else is refused."""
    return _condition(step.get("if"), f"step '{step_name(step)}'")


def continue_on_error(step: dict) -> bool:
    value = step.get("continue-on-error")
    if value is None:
        return False
    if not isinstance(value, bool):
        raise JobError(f"step '{step_name(step)}' has continue-on-error {value!r} (only a literal true or false is read)")
    return value


def timeout_minutes(step: dict) -> int:
    """The step's time limit in whole minutes, 0 for none: the runner casts the number to an integer
    (dropping the fraction) and applies it only above 0."""
    value = step.get("timeout-minutes")
    if value is None:
        return 0
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) \
            or abs(value) > INT32_MAX:
        raise JobError(f"step '{step_name(step)}' has timeout-minutes {value!r} (only a literal number is read)")
    return max(int(value), 0)


def _env_block(node: dict, what: str) -> dict:
    env = node.get("env")
    if env is None:
        return {}
    if not isinstance(env, dict):
        raise JobError(f"{what}'s env: is not a mapping")
    return env


def step_env(job: dict, step: dict) -> dict[str, str]:
    """The job's and the step's env:, as text the way Actions makes it."""
    name = step_name(step)
    out = {}
    for block in (_env_block(job, "the job"), _env_block(step, f"step '{name}'")):
        for k, v in block.items():
            key = actions_text(k, f"step '{name}': an env: name")
            text = actions_text(v, f"step '{name}': env {key}")
            if "${{" in text:
                raise JobError(f"step '{name}' sets {key} from an expression, which this script cannot evaluate")
            out[key] = text
    return out


def upload_paths(step: dict) -> list[str]:
    raw = _literal((step.get("with") or {}).get("path", ""), f"step '{step_name(step)}': path")
    paths = [p.strip() for p in raw.splitlines() if p.strip()]
    if not paths:
        raise JobError(f"step '{step_name(step)}': upload-artifact without a path")
    for p in paths:
        if p.startswith("!") or any(c in p for c in "*?["):
            raise JobError(f"step '{step_name(step)}': path {p} is a pattern; the stand-in copies plain paths only")
        if p.startswith(("/", "~")) or ".." in Path(p).parts:
            raise JobError(f"step '{step_name(step)}': path {p} leaves the workspace; the stand-in copies paths in it only")
    return paths


def upload_mode(step: dict) -> str:
    mode = _literal((step.get("with") or {}).get("if-no-files-found", "warn"),
                    f"step '{step_name(step)}': if-no-files-found")
    if mode not in ("warn", "error", "ignore"):
        raise JobError(f"step '{step_name(step)}': if-no-files-found {mode!r} (warn, error or ignore)")
    return mode


def plan(workflow: dict, job_id: str) -> list[tuple[str, dict]]:
    """The job's steps as (kind, step), after checking the workflow, the job and every step."""
    _only(workflow, WORKFLOW_KEYS, "the workflow")
    jobs = workflow.get("jobs")
    if not isinstance(jobs, dict) or job_id not in jobs:
        raise JobError(f"no job {job_id}")
    job = jobs[job_id]
    if not isinstance(job, dict):
        raise JobError("the job is not a mapping")
    _only(job, JOB_KEYS, "the job")
    _condition(job.get("if"), "the job")
    image = job.get("container")
    if not isinstance(image, str) or not image:
        raise JobError("the job names no container image (container: <image>); this script runs a job in its image only")
    _env_block(job, "the job")
    steps = job.get("steps")
    if not isinstance(steps, list):
        raise JobError("the job has no steps")
    out = []
    for st in steps:
        if not isinstance(st, dict):
            raise JobError(f"a step is not a mapping ({st!r})")
        name = step_name(st)
        if ("run" in st) == ("uses" in st):
            raise JobError(f"step '{name}' needs one of run: and uses:")
        _only(st, RUN_STEP_KEYS if "run" in st else ACTION_STEP_KEYS, f"step '{name}'")
        condition(st)
        continue_on_error(st)
        step_env(job, st)
        if "run" in st:
            _literal(st["run"], f"step '{name}': run")
            shell = _literal(st.get("shell"), f"step '{name}': shell")
            if shell not in ("", "bash"):
                raise JobError(f"step '{name}' uses shell {shell}, not bash")
            _literal(st.get("working-directory"), f"step '{name}': working-directory")
            timeout_minutes(st)
            out.append(("run", st))
            continue
        uses = _literal(st["uses"], f"step '{name}': uses")
        action = uses.split("@", 1)[0]
        if action not in ACTION_INPUTS:
            raise JobError(f"step '{name}' uses {uses}, which this script does not stand in for")
        inputs = st.get("with")
        if inputs is None:
            inputs = {}
        if not isinstance(inputs, dict):
            raise JobError(f"step '{name}': with: is not a mapping")
        extra = sorted(str(k) for k in inputs if k not in ACTION_INPUTS[action])
        if extra:
            raise JobError(f"step '{name}': the stand-in for {action} does not take {', '.join(extra)}")
        if action == "actions/checkout":
            out.append(("checkout", st))
        elif action == "astral-sh/setup-uv":
            version = actions_text(inputs.get("version"), f"step '{name}': version")
            if not re.fullmatch(r"\d+\.\d+\.\d+", version):
                raise JobError(f"step '{name}': setup-uv without a pinned version (found {version!r})")
            out.append(("setup-uv", st))
        else:
            upload_paths(st)
            upload_mode(st)
            out.append(("upload", st))
    if not any(kind == "run" for kind, _ in out):
        raise JobError("the job has no run steps")
    return out


# --- running it ------------------------------------------------------------------------------------


def _visible(path: Path) -> bool:
    return not path.name.startswith(".")


def _visible_files(root: Path) -> list[Path]:
    """What upload-artifact takes from a path by default: no file or directory whose name starts with a
    dot (include-hidden-files: false)."""
    if not _visible(root):
        return []
    if root.is_file():
        return [root]
    files = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if not d.startswith(".")]
        files += [Path(dirpath) / f for f in filenames if not f.startswith(".")]
    return files


class _Output:
    """A step's output (stdout and stderr on one pipe), handed on as it comes, decoded as UTF-8 with
    bad bytes replaced."""

    def __init__(self, stream, sink: Callable[[str], None]):
        self.fd = stream.fileno()
        self.sel = selectors.DefaultSelector()
        self.sel.register(self.fd, selectors.EVENT_READ)
        self.dec = codecs.getincrementaldecoder("utf-8")(errors="replace")
        self.sink = sink
        self.eof = False

    def copy(self, until: float | None = None) -> bool:
        """Copy until the output ends (True) or until time.monotonic() reaches `until` (False)."""
        while not self.eof:
            wait = None if until is None else until - time.monotonic()
            if wait is not None and wait <= 0:
                return False
            if not self.sel.select(wait):
                continue
            chunk = os.read(self.fd, 65536)
            if not chunk:
                self.eof = True
            text = self.dec.decode(chunk, final=not chunk)
            if text:
                self.sink(text)
        return True

    def close(self) -> None:
        self.sel.close()


class Runner:
    # The runner's own sequence for a step it stops (ProcessInvoker): SIGINT, SIGTERM when the step
    # has not ended 7.5 s later, then a kill 2.5 s after that.
    SIGINT_GRACE = 7.5
    SIGTERM_GRACE = 2.5
    SIGKILL_GRACE = 5.0
    MINUTE = 60.0

    def __init__(self, job: dict, log: Path, artifacts: Path | None, workspace: str = WORKSPACE):
        self.job = job
        self.log = open(log, "a", encoding="utf-8", errors="replace")
        self.artifacts = artifacts
        self.workspace = Path(workspace)
        self.tools = Path(tempfile.mkdtemp(prefix="ci-tools-"))
        self.temp = Path(tempfile.mkdtemp(prefix="ci-runner-temp-"))
        self.path_extra: list[str] = []
        self.midline = False

    def close(self) -> None:
        self.log.close()
        shutil.rmtree(self.tools, ignore_errors=True)
        shutil.rmtree(self.temp, ignore_errors=True)

    def echo(self, text: str) -> None:
        sys.stdout.write(text)
        sys.stdout.flush()
        self.log.write(text)
        self.log.flush()
        self.midline = not text.endswith("\n")

    def say(self, text: str) -> None:
        self.echo(("\n" if self.midline else "") + text + "\n")

    def env(self, step: dict) -> dict[str, str]:
        env = dict(os.environ)
        env.update(step_env(self.job, step))
        env.update({"CI": "true", "GITHUB_WORKSPACE": str(self.workspace), "RUNNER_TEMP": str(self.temp)})
        if self.path_extra:
            env["PATH"] = os.pathsep.join(self.path_extra + [env.get("PATH", "")])
        return env

    def run_step(self, step: dict) -> bool:
        name = step_name(step)
        wd = self.workspace / actions_text(step.get("working-directory") or ".", "working-directory")
        shell = actions_text(step.get("shell"), "shell")
        argv = ["bash", "--noprofile", "--norc", "-eo", "pipefail"] if shell == "bash" else ["bash", "-e"]
        with tempfile.NamedTemporaryFile("w", suffix=".sh", dir=self.temp, delete=False) as f:
            f.write(actions_text(step["run"], "run"))
            script = f.name
        try:
            # Its own process group, so that a step past its time limit is stopped with its children.
            proc = subprocess.Popen(argv + [script], cwd=wd, env=self.env(step), stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
        except OSError as e:
            self.say(f"step '{name}': {e}")
            return False
        assert proc.stdout is not None
        out = _Output(proc.stdout, self.echo)
        try:
            minutes = timeout_minutes(step)
            if out.copy(until=time.monotonic() + minutes * self.MINUTE if minutes else None):
                return proc.wait() == 0
            self.say(f"=== step: {name}: timed out after {minutes} minute(s) (timeout-minutes); stopping it")
            self.stop(proc, out)
            return False
        finally:
            out.close()
            proc.stdout.close()

    def stop(self, proc: subprocess.Popen, out: _Output) -> None:
        for sig, grace in ((signal.SIGINT, self.SIGINT_GRACE), (signal.SIGTERM, self.SIGTERM_GRACE),
                           (signal.SIGKILL, self.SIGKILL_GRACE)):
            try:
                os.killpg(proc.pid, sig)
            except ProcessLookupError:
                break
            if self._ended(proc, out, time.monotonic() + grace):
                break
        proc.wait()

    @staticmethod
    def _ended(proc: subprocess.Popen, out: _Output, until: float) -> bool:
        """Copy the step's output until every process of its group has ended (True) or until `until`."""
        while True:
            if out.eof:
                time.sleep(0.05)
            else:
                out.copy(until=min(until, time.monotonic() + 0.1))
            if proc.poll() is not None:
                try:
                    os.killpg(proc.pid, 0)
                except ProcessLookupError:
                    return True
                except PermissionError:
                    pass
            if time.monotonic() >= until:
                return False

    def setup_uv(self, step: dict) -> bool:
        version = actions_text(step["with"]["version"], "version")
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
        paths = upload_paths(step)
        found = 0
        for p in paths:
            src = self.workspace / p
            files = _visible_files(src) if src.exists() else []
            if not files:
                self.say(f"upload-artifact: {p}: no file there")
                continue
            found += len(files)
            if self.artifacts is None:
                continue
            for f in files:
                dst = self.artifacts / p / f.relative_to(src) if src.is_dir() else self.artifacts / p
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(f, dst)
            self.say(f"upload-artifact: {p} ({len(files)} file(s))")
        if not found:
            mode = upload_mode(step)
            if mode == "error":
                self.say("upload-artifact: no files found (if-no-files-found: error)")
                return False
            if mode == "warn":
                self.say("upload-artifact: warning: no files found (if-no-files-found: warn)")
        elif self.artifacts is None:
            self.say("upload-artifact: not kept (no --artifacts directory)")
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
            where = f" (in {actions_text(st.get('working-directory') or '.', 'working-directory')})" \
                if kind == "run" else ""
            self.say(f"=== step: {name}{where}")
            if kind == "run":
                ok = self.run_step(st)
            elif kind == "setup-uv":
                ok = self.setup_uv(st)
            else:
                ok = self.upload(st)
            if ok:
                continue
            if continue_on_error(st):
                self.say(f"=== step: {name}: failed; continue-on-error: the job carries on")
            else:
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
    try:
        workflow = load_workflow(Path(args.workflow).read_text(encoding="utf-8"))
        steps = plan(workflow, args.job)
    except (JobError, yaml.YAMLError) as e:
        print(f"ci_job.py: {args.workflow}, job {args.job}: {e}", file=sys.stderr)
        return 2
    if args.list:
        for kind, st in steps:
            print(f"{kind}: {step_name(st)}")
        return 0
    artifacts = Path(args.artifacts) if args.artifacts else None
    if artifacts is not None:
        artifacts.mkdir(parents=True, exist_ok=True)
    runner = Runner(workflow["jobs"][args.job], Path(args.log), artifacts)
    try:
        return runner.run(steps)
    finally:
        runner.close()


if __name__ == "__main__":
    sys.exit(main())
