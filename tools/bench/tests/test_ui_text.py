# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Sentences the web UI builds from the station's status (components/web/ui/js), run in Node.

The UI's modules are loaded the way the bundle joins them (every js/*.js but
the start-up in 99-main.js, in file-name order, inside one function), with a
document that answers every lookup and timers that never fire, and the
functions under test are called on status objects as the API sends them.
Skipped when Node.js is not installed.
"""

from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
UI_JS = ROOT / "components" / "web" / "ui" / "js"

HARNESS = r"""
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const [dir, casesJson] = process.argv.slice(-2);   /* node -e: no script path in argv */
const files = fs.readdirSync(dir).filter(f => f.endsWith('.js') && f !== '99-main.js').sort();
const src = files.map(f => fs.readFileSync(path.join(dir, f), 'utf8')).join('\n');
const noop = () => {};
const el = new Proxy(function () {}, {
  get: (t, k) => (k === Symbol.toPrimitive ? () => '' : el),
  apply: () => el,
  construct: () => el,
});
const storage = { getItem: () => null, setItem: noop, removeItem: noop };
const ctx = {
  document: { addEventListener: noop, getElementById: () => el, createElement: () => el, createElementNS: () => el,
              createTextNode: () => el, querySelector: () => el, documentElement: el, body: el, hidden: false },
  window: { addEventListener: noop, matchMedia: () => ({ matches: false, addEventListener: noop }) },
  addEventListener: noop, location: { pathname: '/', hash: '', reload: noop }, navigator: {},
  localStorage: storage, sessionStorage: storage,
  setTimeout: noop, clearTimeout: noop, setInterval: noop, clearInterval: noop,
  crypto: globalThis.crypto, console,
};
vm.createContext(ctx);
/* refusalText: what the UI says for an error answer {status, error: {code, ...}} (errText on an ApiError). */
const fns = vm.runInContext('"use strict";\n(function () {\n' + src +
  '\nreturn { restartPlanText, onocoyElsewhereText, connRow, journalDetail,\n' +
  '  refusalText: r => errText(new ApiError(r.status, r.error)) };\n})()', ctx);
const out = JSON.parse(casesJson).map(([fn, arg]) => fns[fn](arg));
process.stdout.write(JSON.stringify(out));
"""


def run_ui(cases: list[tuple[str, object]]) -> list:
    node = shutil.which("node")
    if node is None:
        pytest.skip("Node.js is not installed")
    res = subprocess.run([node, "-e", HARNESS, str(UI_JS), json.dumps(cases)], capture_output=True, text=True,
                         timeout=60)
    assert res.returncode == 0, res.stderr
    return json.loads(res.stdout)


# The hold-overlay station whose replay stopped: the ladder's episode with the admin page open.
HOLD = {"reboot_hold": ["admin"], "reboot_hold_left_s": 1578, "reboot_cap_wait_s": None,
        "reboot_cap_limit": None, "reboot_recheck": False}


def test_restart_plan_only_in_full_recovery():
    full, no_reboot, alerts, unknown = run_ui([
        ("restartPlanText", {**HOLD, "recovery_mode": "full"}),
        ("restartPlanText", {**HOLD, "recovery_mode": "no_reboot"}),
        ("restartPlanText", {**HOLD, "recovery_mode": "alerts_only"}),
        ("restartPlanText", HOLD),
    ])
    assert full is not None and "restarts itself" in full and "Postponed" in full and "27 min" in full
    # The modes without the ESP32 restart never announce one, whatever the station reports.
    assert no_reboot is None
    assert alerts is None
    assert unknown is None


def test_restart_plan_cap_and_recheck_in_full_recovery_only():
    cases = []
    for mode in ("full", "no_reboot", "alerts_only"):
        cases.append(("restartPlanText", {"recovery_mode": mode, "reboot_hold": [], "reboot_hold_left_s": None,
                                          "reboot_cap_wait_s": 900, "reboot_cap_limit": "hour"}))
        cases.append(("restartPlanText", {"recovery_mode": mode, "reboot_hold": [], "reboot_hold_left_s": None,
                                          "reboot_cap_wait_s": None, "reboot_recheck": True}))
    out = run_ui(cases)
    assert out[0] is not None and "Next possible in" in out[0]
    assert out[1] is not None and "after the next search" in out[1]
    assert out[2:] == [None, None, None, None]
    # Nothing pending in full recovery: no sentence either.
    assert run_ui([("restartPlanText", {"recovery_mode": "full", "reboot_hold": []})]) == [None]


def test_onocoy_hint_never_says_set_up_for_a_slot_that_is_not():
    not_ready, off, streams = run_ui([
        ("onocoyElsewhereText", {"slot": 0, "ready": False, "enabled": False}),
        ("onocoyElsewhereText", {"slot": 0, "ready": True, "enabled": False}),
        ("onocoyElsewhereText", {"slot": 2, "ready": True, "enabled": True}),
    ])
    # The Onocoy card on the same page says "Not set up" for a slot without credentials.
    assert "is set up" not in not_ready and "not set up" in not_ready and "slot 1" in not_ready
    assert "credentials" in not_ready
    assert off == "Onocoy is set up in slot 1 and turned off: open it to turn it on."
    assert "slot 3" in streams


def test_no_receiver_refusal_text():
    no_rx, other = run_ui([
        ("refusalText", {"status": 409, "error": {"code": "no_receiver", "message": "No receiver is connected"}}),
        ("refusalText", {"status": 409, "error": {"code": "not_no_receiver", "message": "x"}}),
    ])
    assert no_rx == "No receiver is connected. Connect it, then try again."
    # The detection refusal keeps its own words: the two codes are not the same thing.
    assert other != no_rx and "not looking for a receiver" in other


def test_unknown_rssi_never_shows_dbm():
    """A Wi-Fi drop's RSSI is a reading only from -127 to -1 dBm: null (this build's unknown), 0 and the -128
    of journal records from earlier builds are unknown and show no signal figure."""
    drop = {"kind": "wifi_disconnect", "a": 201}
    out = run_ui([
        ("journalDetail", {**drop, "b": None}),
        ("journalDetail", {**drop, "b": -128}),
        ("journalDetail", {**drop, "b": 0}),
        ("journalDetail", {**drop, "b": -67}),
        ("journalDetail", {**drop, "b": -127}),
        ("journalDetail", {**drop, "b": -1}),
    ])
    for text in out[:3]:
        assert "dBm" not in text and "signal" not in text
        assert "201" in text  # the reason stays
    assert "signal -67 dBm" in out[3]
    assert "signal -127 dBm" in out[4] and "signal -1 dBm" in out[5]


def test_connection_row_rssi():
    row = {"kind": "wifi_disconnect", "reason": 8, "slot": None, "slot_name": None}
    unknown, known, zero, old = run_ui([
        ("connRow", {**row, "rssi": None}),
        ("connRow", {**row, "rssi": -60}),
        ("connRow", {**row, "rssi": 0}),
        ("connRow", {**row, "rssi": -128}),
    ])
    assert unknown[0] == "Wi-Fi dropped" and "dBm" not in unknown[2]
    assert "signal -60 dBm" in known[2]
    assert "dBm" not in zero[2] and "dBm" not in old[2]
