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
  Node: function Node() {},
  document: { addEventListener: noop, getElementById: () => el, createElement: () => el, createElementNS: () => el,
              createTextNode: () => el, querySelector: () => el, documentElement: el, body: el, hidden: false },
  window: { addEventListener: noop, matchMedia: () => ({ matches: false, addEventListener: noop }) },
  addEventListener: noop, location: { pathname: '/', hash: '', reload: noop }, navigator: {},
  localStorage: storage, sessionStorage: storage,
  setTimeout: noop, clearTimeout: noop, setInterval: noop, clearInterval: noop,
  crypto: globalThis.crypto, TextEncoder: globalThis.TextEncoder, console,
};
vm.createContext(ctx);
/* refusalText: what the UI says for an error answer {status, error: {code, ...}} (errText on an ApiError). */
const fns = vm.runInContext('"use strict";\n(function () {\n' + src +
  '\nreturn { restartPlanText, onocoyElsewhereText, connRow, journalDetail, presetInfo: () => ({ PRESETS, PRESET_INFO }),\n' +
  '  onlineShare: a => onlineShare(a[0], a[1]),\n' +
  '  hero: a => { const l = heroLead(a[0], a[1]), t = heroState(a[0], a[1]);\n' +
  '    return { title: t.head, tone: t.tone, key: l.key, line: l.line, alert: l.alertId, notice: l.notice,\n' +
  '      action: l.action ? (l.action.act || l.action.label) : null, primary: !!(l.action && l.action.primary) }; },\n' +
  '  notices: a => noticeItems(a[0], a[1]).map(i => [i.key, i.title]),\n' +
  '  pwProblem: a => adminPasswordProblem(a[0], a[1]), rssi: a => rssiView(a),\n' +
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
    assert "is set up" not in not_ready and "not set up" in not_ready and "credentials" in not_ready
    assert off == "Onocoy is already set up here and turned off. Open it to turn it on."
    assert "already streams to Onocoy" in streams and "one stream per station" in streams
    # The user never sees slot numbers: the note names Onocoy (its button reads "Open Onocoy").
    for text in (not_ready, off, streams):
        assert "slot" not in text.lower()


def test_every_caster_choice_has_its_box():
    """Every caster in the editor's list has a box: one line saying what it is, 1 to 4 short steps, and a link
    to the caster's own guide (Other caster has no guide). Nothing that goes stale: no dates."""
    (got,) = run_ui([("presetInfo", None)])
    ids = [p["id"] for p in got["PRESETS"]]
    info = got["PRESET_INFO"]
    assert sorted(ids) == sorted(info), (ids, list(info))
    for pid, p in info.items():
        assert p["what"] and p["what"].endswith("."), pid
        assert 1 <= len(p["steps"]) <= 4, pid
        assert all(s.endswith(".") and len(s) <= 130 for s in p["steps"]), pid
        text = " ".join([p["what"], *p["steps"]])
        assert not any(m in text for m in ("2025", "2026", "2027", "as of", "Rev1 =", "kbit", "Mbit")), pid
        if pid == "custom":
            assert p["guide"] is None
        else:
            label, url = p["guide"]
            assert label and url.startswith(("https://", "http://rtk2go.com/")), pid


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


def test_today_online_share_on_the_dashboard():
    """Whole numbers at 0 % and 100 % (no "0.00 %"), two decimals in between, rounded down: never "100.00 %"
    while the caster was offline for a moment."""
    out = run_ui([("onlineShare", [0, 600]), ("onlineShare", [600, 0]), ("onlineShare", [1, 2]),
                  ("onlineShare", [86399, 1])])
    assert out == ["0\u00a0%", "100\u00a0%", "33.33\u00a0%", "99.99\u00a0%"], out


# ---- The Status page's top card (heroLead, heroState) and the notices it covers (noticeItems) ----

def caster(slot: int, name: str, state: str, **kw) -> dict:
    c = {"slot": slot, "name": name, "empty": False, "enabled": state != "disabled", "protected": name == "Onocoy",
         "host": "servers.onocoy.com" if name == "Onocoy" else name.lower() + ".example", "state": state, "reason": "none",
         "next_retry_s": 0, "stop_reason": "none", "onocoy_ready": name == "Onocoy"}
    c.update(kw)
    return c


def status(**kw) -> dict:
    """An admin GET /api/status: the receiver streams, Onocoy streams, nothing is wrong."""
    s = {"api": 1, "public": False, "uptime_s": 23520,
         "device": {"hostname": "onocoydiy", "settings_available": True},
         "safe_mode": {"active": False, "retry_in_s": None, "crashes": 0},
         "network": {"online": True, "ssid": "Home"},
         "receiver": {"state": "streaming", "rtcm_age_ms": 180, "journal": "none", "uart_closed": False,
                      "rx_maintenance": False, "maintenance_left_s": 0, "first_apply_held": False},
         "casters": [caster(0, "Onocoy", "streaming")],
         "health": {"level": "ok", "last_step": "none", "recovery_mode": "full"},
         "alerts": [], "update": {}}
    for k, v in kw.items():
        if isinstance(v, dict) and isinstance(s.get(k), dict):
            s[k] = {**s[k], **v}
        else:
            s[k] = v
    return s


def heroes(*statuses, admin: bool = True) -> list[dict]:
    return run_ui([("hero", [st, admin]) for st in statuses])


def test_hero_says_what_blocks_first_and_what_to_do():
    onocoy_not_set_up = status(casters=[caster(0, "Onocoy", "disabled", onocoy_ready=False)],
                               health={"level": "degraded", "recovery_mode": "alerts_only"})
    maintenance = status(receiver={"state": "idle", "rx_maintenance": True, "maintenance_left_s": 830})
    # The QEMU run: no receiver found while the recovery still reports recovering.
    no_rx = status(receiver={"state": "no_receiver", "rtcm_age_ms": None}, health={"level": "recovering", "last_step": "rescan"},
                   alerts=[{"id": "no_receiver", "source": "station", "saw_bytes": False, "next_detect_in_s": 24}],
                   casters=[caster(0, "Onocoy", "wait_data")])
    stall = status(receiver={"rtcm_age_ms": 30000}, health={"level": "recovering", "last_step": "link_restart"})
    alerts_only = status(health={"level": "degraded", "recovery_mode": "alerts_only"},
                         alerts=[{"id": "no_rtcm", "source": "health"}], casters=[caster(0, "Onocoy", "wait_data")])
    out = heroes(onocoy_not_set_up, maintenance, no_rx, stall, alerts_only)

    assert out[0]["title"] == "No caster turned on" and out[0]["tone"] == "warn", out[0]
    assert out[0]["line"] == "Add your Onocoy credentials to start streaming."
    assert out[0]["action"] == "Set up Onocoy" and out[0]["primary"] is True

    assert out[1]["title"] == "Receiver maintenance"
    assert out[1]["line"] == "The station sends the receiver no commands. Ends in about 14 min."
    assert out[1]["action"] == "end-maint" and out[1]["notice"] == "maintenance"

    assert out[2]["title"] == "No receiver found" and out[2]["tone"] == "bad", out[2]
    assert out[2]["line"].startswith("Nothing arrives from the receiver. Check its power") and "Next search in 24 s." in out[2]["line"]
    assert out[2]["action"] == "detect" and out[2]["alert"] == "no_receiver"

    # A stall while the station still says streaming: never "Streaming" on top.
    assert out[3]["title"] == "Fixing the receiver" and out[3]["tone"] == "warn", out[3]
    assert out[3]["line"] == "Corrections from the receiver stopped. The station is fixing it: reconnecting to the receiver."

    assert out[4]["title"] == "Not streaming" and out[4]["alert"] == "no_rtcm", out[4]
    assert out[4]["line"] == "The receiver answers but sends no corrections. Check that the antenna is on the socket marked M and sees the open sky."

    # No health level and no setting name on top, ever.
    for o in out:
        text = (o["title"] + " " + o["line"]).lower()
        assert not any(w in text for w in ("alerts only", "needs attention", "healthy", "recovery:", "degraded")), o


def test_hero_casters_down_one_two_three_and_all_good():
    one = status(casters=[caster(0, "Onocoy", "streaming"),
                          caster(1, "Bench", "auth_failed", reason="auth", next_retry_s=30)])
    two = status(casters=[caster(0, "Onocoy", "backoff", reason="timeout"), caster(1, "Bench", "auth_failed", reason="auth")])
    three = status(casters=[caster(0, "Onocoy", "connecting"), caster(1, "Bench", "backoff", reason="dns"),
                            caster(2, "RTK2go", "wait_network")])
    good_one = status()
    good_two = status(casters=[caster(0, "Onocoy", "streaming"), caster(1, "RTK2go", "streaming")])
    waiting = status(casters=[caster(0, "Onocoy", "wait_data"), caster(1, "RTK2go", "wait_data")])
    dns = status(casters=[caster(0, "Onocoy", "streaming"), caster(1, "Bench", "backoff", reason="dns")])
    out = heroes(one, two, three, good_one, good_two, waiting, dns)
    assert out[0]["title"] == "Partly streaming"
    assert out[0]["line"] == "Bench is not streaming: login refused. Next try in 30 s." and out[0]["action"] == "Casters"
    assert out[1]["title"] == "Not streaming" and out[1]["tone"] == "bad"
    assert out[1]["line"] == "Onocoy and Bench are not streaming."
    assert out[2]["line"] == "3 casters are not streaming."
    assert out[3] == {**out[3], "title": "Streaming", "tone": "ok", "line": "Onocoy is receiving corrections.", "action": None}
    assert out[4]["line"] == "Onocoy and RTK2go are receiving corrections."
    assert out[5]["line"] == "Waiting for corrections from the receiver."
    assert out[6]["line"] == "Bench is not streaming: DNS lookup failed."
    # The public form: states only, no reason, no button.
    pub = heroes({"api": 1, "public": True, "uptime_s": 60, "device": {"hostname": "onocoydiy"},
                  "receiver": {"state": "streaming", "best_bands": 4},
                  "casters": [{"slot": 0, "name": "Onocoy", "state": "streaming"}, {"slot": 1, "name": "Bench", "state": "backoff"}],
                  "health": {"level": "ok"}}, admin=False)[0]
    assert pub["line"] == "Bench is not streaming." and pub["action"] is None and pub["title"] == "Partly streaming"


def test_hero_safe_mode_stop_and_alerts_while_streaming():
    safe = status(safe_mode={"active": True, "crashes": 3, "retry_in_s": 2690}, receiver={"state": "idle"},
                  casters=[caster(0, "Onocoy", "disabled")], alerts=[{"id": "safe_mode", "source": "health"}])
    stopped = status(casters=[caster(0, "Onocoy", "stopped", stop_reason="duplicate")])
    link_loss = status(alerts=[{"id": "link_loss", "source": "health"}])
    bare_level = status(health={"level": "degraded"})
    off = status(casters=[caster(0, "Onocoy", "disabled", enabled=False), caster(1, "RTK2go", "disabled", enabled=False, onocoy_ready=False)])
    none = status(casters=[])
    out = heroes(safe, stopped, link_loss, bare_level, off, none)
    assert out[0]["title"] == "Safe mode" and out[0]["tone"] == "bad"
    assert out[0]["line"] == "The receiver and casters are off after 3 crashes. The station tries normal operation again in 45 min."
    assert out[0]["alert"] == "safe_mode" and out[0]["notice"] == "safe_mode"
    assert out[1]["line"] == "Onocoy stopped: duplicate stream. It makes no new attempt until you act."
    assert out[1]["action"] == "What to do" and out[1]["notice"] == "onocoy_stopped:0"
    # An alert while everything streams: its text on top, and the dot turns amber.
    assert out[2]["title"] == "Streaming" and out[2]["tone"] == "warn" and out[2]["alert"] == "link_loss"
    assert out[2]["line"].startswith("Some data from the receiver was lost") and out[2]["action"] == "Diagnostics"
    # The level alone (no alert to show) leaves the dot green.
    assert out[3]["tone"] == "ok" and out[3]["line"] == "Onocoy is receiving corrections."
    assert out[4]["line"] == "Onocoy is set up but turned off. Turn it on to start streaming." and out[4]["action"] == "Turn Onocoy on"
    assert out[5]["line"] == "Add a caster to start streaming." and out[5]["action"] == "Add a caster"


def test_status_page_leaves_out_the_notice_the_top_card_says():
    maint = status(receiver={"state": "idle", "rx_maintenance": True, "maintenance_left_s": 600},
                   update={"app_available": True, "app_version": "v0.4.0"})
    stopped = status(casters=[caster(0, "Onocoy", "stopped", stop_reason="duplicate")])
    settings = status(device={"settings_available": False}, alerts=[{"id": "settings_degraded", "source": "health"}])
    # The hero says the casters are down: the settings notice stays on the Status page.
    settings_down = status(device={"settings_available": False}, alerts=[{"id": "settings_degraded", "source": "health"}],
                           casters=[caster(0, "Onocoy", "backoff", reason="timeout")])
    out = run_ui([("notices", [maint, ""]), ("notices", [maint, "casters"]), ("notices", [stopped, ""]),
                  ("notices", [stopped, "receiver"]), ("notices", [settings, ""]), ("notices", [settings, "diag"]),
                  ("notices", [settings_down, ""])])
    keys = [[k for k, _ in o] for o in out]
    assert keys[0] == ["update"], out[0]
    assert keys[1] == ["maintenance", "update"]
    assert keys[2] == [] and keys[3] == ["onocoy_stopped:0"]
    assert out[3][0][1] == "Onocoy stopped: duplicate stream"
    assert keys[4] == [] and keys[5] == ["settings"] and keys[6] == ["settings"]
    assert out[0][0][1] == "Update available"


def test_password_mismatch_and_unknown_signal():
    mismatch, short, ok = run_ui([("pwProblem", ["abcdefgh", "abcdefgi"]), ("pwProblem", ["abc", "abc"]),
                                  ("pwProblem", ["abcdefgh", "abcdefgh"])])
    # The setup step and the System page pick the repeat field by this exact text.
    assert mismatch == "The passwords do not match."
    assert short == "Use at least 8 characters." and ok is None
    assert run_ui([("rssi", 0), ("rssi", None)]) == ["–", "–"]
