# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""tools/ci/ci_job.py, which runs a job of build.yml as CI's runner does: build.yml read as Actions
reads it (YAML 1.2 core types, values made text as Actions makes them), everything it would not run
as CI does refused before the first step, and continue-on-error, timeout-minutes, if: always() and
the upload stand-in's if-no-files-found applied, over small jobs run here with bash."""

from __future__ import annotations

import importlib.util
import math
import os
import sys
import tempfile
import textwrap
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
BUILD_YML = ROOT / ".github" / "workflows" / "build.yml"


def _load():
    spec = importlib.util.spec_from_file_location("ci_job_under_test", ROOT / "tools" / "ci" / "ci_job.py")
    mod = importlib.util.module_from_spec(spec)
    before = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec.loader.exec_module(mod)
    finally:
        sys.dont_write_bytecode = before
    return mod


ci = _load()

IMAGE = "espressif/idf@sha256:" + "0" * 64


def workflow(job_yaml: str, top: str = "") -> dict:
    """A workflow with one job, `j`, from the job's YAML (indented as under `jobs: j:`)."""
    text = f"name: t\non:\n  push:\n{top}jobs:\n  j:\n" + textwrap.indent(textwrap.dedent(job_yaml), "    ")
    return ci.load_workflow(text)


def job(steps_yaml: str, extra: str = "") -> dict:
    return workflow(f"runs-on: ubuntu-24.04\ncontainer: {IMAGE}\n{extra}steps:\n"
                    + textwrap.indent(textwrap.dedent(steps_yaml), "  "))


def refused(wf: dict) -> str:
    with pytest.raises(ci.JobError) as e:
        ci.plan(wf, "j")
    return str(e.value)


# --- build.yml as Actions reads it ---------------------------------------------------------------


def test_plain_scalars_take_the_yaml_1_2_core_types():
    data = ci.load_workflow(textwrap.dedent("""\
        on: push
        jobs: {}
        v:
          yes_word: yes
          off_word: off
          sexagesimal: 1:30
          date: 2026-10-13
          underscored: 1_000
          leading_zero: 010
          hex: 0x1F
          octal: 0o17
          t1: true
          t2: True
          t3: TRUE
          f: false
          n1: ~
          n2: null
          n3:
          quoted_true: "true"
          quoted_num: '1.0'
          block: |
            true
          float: 1.50
          exp: 1e3
          dot: .5
          inf: -.inf
          nan: .NaN
        """))
    assert "on" in data and True not in data
    v = data["v"]
    assert v["yes_word"] == "yes" and v["off_word"] == "off"
    assert v["sexagesimal"] == "1:30" and v["date"] == "2026-10-13" and v["underscored"] == "1_000"
    assert v["leading_zero"] == 10.0 and v["hex"] == 31.0 and v["octal"] == 15.0
    assert v["t1"] is True and v["t2"] is True and v["t3"] is True and v["f"] is False
    assert v["n1"] is None and v["n2"] is None and v["n3"] is None
    assert v["quoted_true"] == "true" and v["quoted_num"] == "1.0" and v["block"] == "true\n"
    assert v["float"] == 1.5 and v["exp"] == 1000.0 and v["dot"] == 0.5
    assert v["inf"] == -math.inf and math.isnan(v["nan"])


@pytest.mark.parametrize("text", ["0x80000000", "0o20000000000", "1" * 400, "1e999"])
def test_numbers_beyond_what_actions_reads_as_written_are_refused(text):
    with pytest.raises(ci.JobError, match="beyond"):
        ci.load_workflow(f"jobs: {{}}\nv: {text}\n")


@pytest.mark.parametrize("value, text", [
    (True, "true"), (False, "false"), (None, ""), ("True", "True"),
    (1.0, "1"), (1.5, "1.5"), (-2.0, "-2"), (0.1 + 0.2, "0.3"), (1e20, "1E+20"), (1e15, "1E+15"),
    (123456789012345.0, "123456789012345"), (12345678901234567890.0, "1.23456789012346E+19"),
    (0.0001, "0.0001"), (0.00001, "1E-05"), (math.inf, "Infinity"), (-math.inf, "-Infinity"), (math.nan, "NaN"),
])
def test_values_become_text_as_actions_makes_it(value, text):
    assert ci.actions_text(value, "v") == text


def test_env_values_reach_a_step_as_actions_gives_them():
    wf = job("""\
        - name: s
          env:
            STEP: true
            NUM: 1.0
            OCT: 010
            YES: yes
            NONE: ~
            QUOTED: "True"
          run: "true"
        """, extra="env:\n  JOB: false\n  STEP: job\n")
    j = wf["jobs"]["j"]
    assert ci.step_env(j, j["steps"][0]) == {"JOB": "false", "STEP": "true", "NUM": "1", "OCT": "10", "YES": "yes",
                                             "NONE": "", "QUOTED": "True"}


# --- what is refused before the first step ---------------------------------------------------------

STEP = "- run: 'true'\n"


@pytest.mark.parametrize("wf, words", [
    (lambda: workflow(f"runs-on: x\ncontainer: {IMAGE}\nsteps:\n  {STEP}", top="env:\n  STRICT: true\n"),
     "the workflow sets env"),
    (lambda: workflow(f"runs-on: x\ncontainer: {IMAGE}\nsteps:\n  {STEP}",
                      top="defaults:\n  run:\n    working-directory: sub\n"),
     "the workflow sets defaults"),
    (lambda: job(STEP, extra="strategy:\n  matrix:\n    a: [1, 2]\n"), "the job sets strategy"),
    (lambda: job(STEP, extra="timeout-minutes: 30\n"), "the job sets timeout-minutes"),
    (lambda: job(STEP, extra="continue-on-error: true\n"), "the job sets continue-on-error"),
    (lambda: job(STEP, extra="services:\n  db:\n    image: x\n"), "the job sets services"),
    (lambda: job(STEP, extra="defaults:\n  run:\n    shell: bash\n"), "the job sets defaults"),
    (lambda: job(STEP, extra="environment: prod\n"), "the job sets environment"),
    (lambda: job(STEP, extra="if: github.event_name == 'push'\n"), "the job has the condition"),
    (lambda: workflow(f"runs-on: x\ncontainer:\n  image: {IMAGE}\n  env:\n    A: b\nsteps:\n  {STEP}"),
     "names no container image"),
    (lambda: workflow(f"runs-on: x\nsteps:\n  {STEP}"), "names no container image"),
    (lambda: job("- run: 'true'\n  timeout-minute: 5\n"), "sets timeout-minute,"),
    (lambda: job("- run: 'true'\n  with:\n    a: b\n"), "sets with,"),
    (lambda: job("- uses: actions/checkout@v7\n  working-directory: sub\n- run: 'true'\n"), "sets working-directory"),
    (lambda: job("- uses: actions/checkout@v7\n  timeout-minutes: 5\n- run: 'true'\n"), "sets timeout-minutes"),
    (lambda: job("- run: 'true'\n  uses: actions/checkout@v7\n"), "one of run: and uses:"),
    (lambda: job("- run: echo ${{ github.sha }}\n"), "run has an expression"),
    (lambda: job("- run: 'true'\n  working-directory: ${{ github.workspace }}\n"), "working-directory has an expression"),
    (lambda: job("- run: 'true'\n  env:\n    A: ${{ github.sha }}\n"), "sets A from an expression"),
    (lambda: job("- run: 'true'\n  env: ${{ fromJSON('{}') }}\n"), "env: is not a mapping"),
    (lambda: job("- run: 'true'\n  shell: sh\n"), "shell sh"),
    (lambda: job("- run: 'true'\n  if: failure()\n"), "the condition"),
    (lambda: job("- run: 'true'\n  continue-on-error: ${{ matrix.experimental }}\n"), "continue-on-error"),
    (lambda: job("- run: 'true'\n  continue-on-error: 'true'\n"), "continue-on-error"),
    (lambda: job("- run: 'true'\n  timeout-minutes: ${{ inputs.t }}\n"), "timeout-minutes"),
    (lambda: job("- run: 'true'\n  timeout-minutes: '30'\n"), "timeout-minutes"),
    (lambda: job("- run: 'true'\n  timeout-minutes: .inf\n"), "timeout-minutes"),
    (lambda: job("- uses: actions/cache@v4\n- run: 'true'\n"), "does not stand in for"),
    (lambda: job("- uses: actions/checkout@v7\n  with:\n    path: sub\n- run: 'true'\n"), "does not take path"),
    (lambda: job("- uses: astral-sh/setup-uv@v10.2.0\n  with:\n    version: '0.12.24'\n    python-version: '3.12'\n"
                 "- run: 'true'\n"), "does not take python-version"),
    (lambda: job("- uses: astral-sh/setup-uv@v10.2.0\n- run: 'true'\n"), "pinned version"),
    (lambda: job("- run: 'true'\n- uses: actions/upload-artifact@v7\n  with:\n    path: logs/*.txt\n"), "a pattern"),
    (lambda: job("- run: 'true'\n- uses: actions/upload-artifact@v7\n  with:\n    path: '!x'\n"), "a pattern"),
    (lambda: job("- run: 'true'\n- uses: actions/upload-artifact@v7\n  with:\n    path: /etc/passwd\n"), "leaves the workspace"),
    (lambda: job("- run: 'true'\n- uses: actions/upload-artifact@v7\n  with:\n    path: out/../../x\n"), "leaves the workspace"),
    (lambda: job("- run: 'true'\n- uses: actions/upload-artifact@v7\n  with:\n    name: n\n"), "without a path"),
    (lambda: job("- run: 'true'\n- uses: actions/upload-artifact@v7\n  with:\n    path: x\n    if-no-files-found: fail\n"),
     "if-no-files-found"),
    (lambda: job("- run: 'true'\n- uses: actions/upload-artifact@v7\n  with:\n    path: x\n    include-hidden-files: true\n"),
     "does not take include-hidden-files"),
    (lambda: job("- uses: actions/checkout@v7\n"), "no run steps"),
])
def test_refused_before_the_first_step(wf, words):
    assert words in refused(wf())


def test_what_it_reproduces_is_accepted():
    wf = job("""\
        - uses: actions/checkout@v7
          with:
            fetch-depth: 0
        - uses: astral-sh/setup-uv@v10.2.0
          with:
            version: "0.12.24"
        - name: build
          id: b
          shell: bash
          working-directory: sub
          env:
            A: b
          continue-on-error: true
          timeout-minutes: 30
          run: make
        - name: upload
          if: ${{ always() }}
          continue-on-error: false
          uses: actions/upload-artifact@v7
          with:
            name: logs-${{ github.sha }}
            if-no-files-found: error
            retention-days: 3
            path: |
              build.log
              out/
        """, extra="if: always()\nneeds: [x]\npermissions:\n  contents: read\nconcurrency: c\nenv:\n  J: 1\n")
    assert [kind for kind, _ in ci.plan(wf, "j")] == ["checkout", "setup-uv", "run", "upload"]


@pytest.mark.parametrize("value, minutes", [(30, 30), (1.9, 1), (0.9, 0), (0, 0), (-5, 0), (None, 0)])
def test_timeout_minutes_counted_as_the_runner_counts_them(value, minutes):
    step = {"run": "true"} if value is None else {"run": "true", "timeout-minutes": float(value)}
    assert ci.timeout_minutes(step) == minutes


def test_build_yml_jobs_run_here_are_accepted():
    wf = ci.load_workflow(BUILD_YML.read_text(encoding="utf-8"))
    assert "on" in wf
    kinds = {name: [kind for kind, _ in ci.plan(wf, name)] for name in ("host-tests", "qemu-smoke", "qemu-bench-console")}
    assert kinds["host-tests"] == ["checkout", "run"]
    assert kinds["qemu-smoke"] == ["checkout", "setup-uv", "run", "run", "run", "upload"]
    assert kinds["qemu-bench-console"] == ["checkout", "setup-uv", "run", "run", "upload"]


# --- running a job -----------------------------------------------------------------------------------


@pytest.fixture
def run_job(tmp_path, monkeypatch):
    """Run job `j` of a workflow in tmp_path/work: (exit status, log text, seconds taken, workspace)."""
    monkeypatch.setattr(tempfile, "tempdir", str(tmp_path))
    work = tmp_path / "work"
    work.mkdir()

    def run(wf: dict, artifacts: Path | None = None, **timing):
        log = tmp_path / "steps.log"
        runner = ci.Runner(wf["jobs"]["j"], log, artifacts, workspace=str(work))
        for k, v in timing.items():
            setattr(runner, k, v)
        start = time.monotonic()
        try:
            rc = runner.run(ci.plan(wf, "j"))
        finally:
            runner.close()
        assert not runner.temp.exists() and not runner.tools.exists()
        return rc, log.read_text(encoding="utf-8"), time.monotonic() - start, work

    return run


def test_a_failed_step_skips_the_rest_but_always(run_job):
    rc, log, _, work = run_job(job("""\
        - name: one
          run: echo first; exit 3
        - name: two
          run: touch two
        - name: three
          if: always()
          run: touch three
        """))
    assert rc == 1
    assert "=== step: one: FAILED" in log and "=== step: two: skipped" in log
    assert not (work / "two").exists() and (work / "three").exists()


def test_continue_on_error_carries_the_job_on(run_job):
    rc, log, _, work = run_job(job("""\
        - name: may fail
          continue-on-error: true
          run: exit 3
        - name: next
          run: touch next
        """))
    assert rc == 0
    assert "may fail: failed; continue-on-error" in log and (work / "next").exists()


def test_step_env_and_working_directory(run_job, tmp_path):
    (tmp_path / "work" / "sub").mkdir()
    rc, log, _, _ = run_job(job("""\
        - name: env
          shell: bash
          working-directory: sub
          env:
            FLAG: true
            NUM: 2.50
          run: |
            [ "$FLAG" = true ] && [ "$NUM" = 2.5 ] && [ "$(basename "$PWD")" = sub ]
            [ "$GITHUB_WORKSPACE" = "$(cd .. && pwd -P)" ] || [ "$GITHUB_WORKSPACE" = "$(cd .. && pwd)" ]
        """))
    assert rc == 0, log


def test_output_and_the_next_step_line_stay_apart(run_job):
    rc, log, _, _ = run_job(job("""\
        - name: partial
          run: printf 'no newline'
        - name: after
          run: printf 'caf\\xc3\\xa9 \\xff\\n'
        """))
    assert rc == 0
    assert "no newline\n=== step: after" in log
    assert "café \ufffd\n" in log


def test_a_step_past_its_time_limit_fails_and_its_children_stop(run_job, tmp_path):
    rc, log, took, work = run_job(job("""\
        - name: hangs
          timeout-minutes: 1
          run: |
            sleep 300 &
            echo $! > child.pid
            echo started
            wait
        - name: logs
          if: always()
          run: touch logs
        - name: skipped
          run: touch skipped
        """), MINUTE=0.5, SIGINT_GRACE=1.0)  # a background job of a script ignores SIGINT: SIGTERM stops it
    assert rc == 1
    assert "started" in log and "hangs: timed out after 1 minute(s)" in log and "=== step: hangs: FAILED" in log
    assert took < 10
    assert (work / "logs").exists() and not (work / "skipped").exists()
    _assert_gone(int((work / "child.pid").read_text()))


def test_a_step_ignoring_sigint_and_sigterm_is_killed(run_job):
    rc, log, took, work = run_job(job("""\
        - name: stubborn
          timeout-minutes: 1
          shell: bash
          run: |
            trap '' INT TERM
            sleep 300 &
            echo $! > child.pid
            wait
        """), MINUTE=0.3, SIGINT_GRACE=0.3, SIGTERM_GRACE=0.3)
    assert rc == 1 and "timed out" in log
    assert took < 10
    _assert_gone(int((work / "child.pid").read_text()))


def test_continue_on_error_covers_a_timeout(run_job):
    rc, log, _, work = run_job(job("""\
        - name: slow
          timeout-minutes: 1
          continue-on-error: true
          run: sleep 300
        - name: next
          run: touch next
        """), MINUTE=0.3)
    assert rc == 0 and "slow: failed; continue-on-error" in log and (work / "next").exists()


def test_a_step_within_its_time_limit_passes(run_job):
    rc, log, _, _ = run_job(job("""\
        - name: quick
          timeout-minutes: 1
          run: echo done
        """), MINUTE=5.0)
    assert rc == 0 and "done" in log and "timed out" not in log


UPLOAD = """\
- name: make
  run: |
    mkdir -p out/.cache out/sub
    echo log > build.log
    echo a > out/a.txt
    echo b > out/sub/b.txt
    echo hidden > out/.hidden
    echo cached > out/.cache/c
- name: upload
  uses: actions/upload-artifact@v7
  with:
    name: logs
    if-no-files-found: {mode}
    path: |
      {paths}
"""


def test_upload_copies_what_the_action_takes(run_job, tmp_path):
    arts = tmp_path / "arts"
    rc, log, _, _ = run_job(job(UPLOAD.format(mode="error", paths="build.log\n      out/\n      missing.txt")), arts)
    assert rc == 0, log
    got = sorted(str(p.relative_to(arts)) for p in arts.rglob("*") if p.is_file())
    assert got == ["build.log", "out/a.txt", "out/sub/b.txt"]
    assert "missing.txt: no file there" in log


@pytest.mark.parametrize("mode, rc, words", [
    ("error", 1, "no files found (if-no-files-found: error)"),
    ("warn", 0, "warning: no files found"),
    ("ignore", 0, None),
])
def test_upload_with_nothing_to_take(run_job, tmp_path, mode, rc, words):
    got_rc, log, _, _ = run_job(job(UPLOAD.format(mode=mode, paths="missing.txt\n      out/.cache")), tmp_path / "arts")
    assert got_rc == rc, log
    if words:
        assert words in log
    else:
        assert "no files found" not in log


def _assert_gone(pid: int) -> None:
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return
        time.sleep(0.05)
    os.kill(pid, 9)
    pytest.fail(f"process {pid} outlived its step")
