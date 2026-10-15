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
import re
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
  '  connItems: l => connItems(l), journalItems: a => journalItems(a[0], a[1], a[2]), evFilter: () => EV_FILTER,\n' +
  '  wifiChip: w => wifiChip(w), recoveryHint: () => recoveryHint(),\n' +
  '  onlineShare: a => onlineShare(a[0], a[1]),\n' +
  '  hero: a => { const t = stationState(a[0], a[1]);\n' +
  '    return { title: t.head, tone: t.tone, key: t.key, line: t.lead, alert: t.alertId, notice: t.notice, trial: t.trial,\n' +
  '      action: t.actions.length ? (t.actions[0].act || t.actions[0].label) : null, actions: t.actions.map(x => x.act || x.label),\n' +
  '      primary: t.actions.some(x => x.primary), also: t.also.map(x => x.id), alsoText: t.also.map(x => x.title + \'. \' + x.text),\n' +
  '      facts: t.facts, delivering: t.delivering, ono: t.onoStreaming, quality: t.quality,\n' +
  '      subject: t.subject, plan: t.plan }; },\n' +
  '  onocoy: a => { const t = stationState(a, true), d = onocoyDay(a, t);\n' +
  '    return d && { verdict: d.verdict, tone: d.tone, why: d.why, left: d.left, used: d.used, hero: t.key }; },\n' +
  '  checks: d => receptionChecks(d),\n' +
  '  notices: a => noticeItems(a[0], a[1]).map(i => [i.key, i.title]),\n' +
  '  noticeLines: a => noticeItems(a[0], a[1]).map(i => [i.key, i.tone, i.urgent, i.title + (i.text ? \'. \' + i.text : \'\'),\n' +
  '    Array.isArray(i.actions) ? i.actions.length : i.actions ? 1 : 0]),\n' +
  '  fold: a => { const f = foldNotices(noticeItems(a[0], a[1]), a[2]); return [f.shown.map(i => i.key), f.more]; },\n' +
  '  moved: a => movedTo(a[0], a[1]), tab: n => tabOf(n), nav: () => NAV.map(n => n[1]),\n' +
  '  pwProblem: a => adminPasswordProblem(a[0], a[1]), rssi: a => rssiView(a),\n' +
  '  sgName: a => sgName(a[0], a[1]), sgChoices: () => SG_CHOICES, recovery: () => L.recovery,\n' +
  '  recoveryLbl: k => lbl(L.recovery, k), fwText: a => fwText(a[0], a[1]), dist: m => fmtDist(m),\n' +
  '  sgtBlock: g => sgtBlock(g), sgtLine: t => sgtLine(t), profileChip: g => profileChip(g),\n' +
  '  sgtNotice: st => { const n = sgTestNotice(st); return n && n.slice(0, 3); },\n' +
  '  rxChip: s => receiverChip(s), posLine: a => positionLine(a[0], a[1]), statusPos: a => positionText(a[0], a[1]),\n' +
  '  headline: sig => signalsHeadline(sig), sigChecks: sig => signalChecks(sig), msgCheck: a => msgCheck(a[0], a[1], a[2]),\n' +
  '  manage: () => MANAGE, manageKey: a => manageKey(a[0], a[1]), manageCustom: a => manageCustom(a[0], a[1]),\n' +
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
    assert unknown[:2] == ["Wi-Fi dropped", "left the network (8)"]
    assert known[1] == "left the network (8), signal -60 dBm"
    assert "dBm" not in zero[1] and "dBm" not in old[1]


def conn(seq: int, kind: str, **kw) -> dict:
    e = {"seq": seq, "time_unix": None, "uptime_s": seq * 10, "kind": kind, "slot": None, "slot_name": None, "class": None,
         "count": None, "duration_s": None, "reason": None, "rssi": None, "stop_reason": None}
    e.update(kw)
    return e


def test_connection_history_pairs_a_drop_with_its_return_and_folds_repeats():
    """Newest first, as GET /api/connections sends it: a drop and the return after it are one row, the same row
    three times in a row is one row with a "x3" toggle; a drop without a return stays as it is."""
    onocoy = {"slot": 0, "slot_name": "Onocoy"}
    pairs = []
    for i in range(3):
        pairs = [conn(10 + 2 * i + 1, "caster_up", duration_s=20, count=2, **onocoy),
                 conn(10 + 2 * i, "caster_down", duration_s=3600, **{"class": "remote_close"}, **onocoy)] + pairs
    hist = ([conn(30, "caster_down", duration_s=60, **{"class": "timeout"}, **onocoy),
             conn(21, "wifi_up", duration_s=40, count=3), conn(20, "wifi_disconnect", reason=202, rssi=-67)]
            + pairs + [conn(5, "caster_stop", stop_reason="duplicate", **onocoy)])
    (rows,) = run_ui([("connItems", hist)])
    assert [r["ev"] for r in rows] == ["Onocoy disconnected", "Wi-Fi dropped", "Onocoy disconnected", "Onocoy stopped"]
    assert rows[0]["det"] == "after 1\u00a0min online, timed out" and rows[0]["tone"] == "warn"
    assert rows[1]["det"] == "back after 40\u00a0s, authentication failed (wrong password?) (202), signal -67 dBm"
    assert rows[2]["det"] == "back after 20\u00a0s, closed by the caster, 2 failed attempts"
    assert len(rows[2]["all"]) == 3 and rows[2]["id"] == "r10"
    assert rows[3]["det"] == "duplicate stream, no new attempt until you act" and rows[3]["tone"] == "bad"
    for r in rows:
        assert "slot" not in (r["ev"] + r["det"]).lower()


def jrec(seq: int, kind: str, a: int = 0, b: int = 0, up: int = 1, t: int | None = None) -> dict:
    return {"seq": seq, "time_unix": t, "uptime_s": up, "kind": kind, "a": a, "b": b}


def test_event_journal_one_heading_per_start_and_repeats_folded():
    """A start is one heading (its boot and session records, and the restart record that announced it, folded in),
    its other records newest first, 27 recovery steps in a row one row with a toggle; no "#seq" or "≈" in the text."""
    t0 = 1760500000
    first = [jrec(1, "boot", 0, 0, 1), jrec(2, "session", -1, 1, 1)]
    steps = [jrec(3 + i, "ladder_step", 0, (0 << 8) | 2, 60 + i, t0 + 60 + i) for i in range(27)]
    second = [jrec(30, "reboot", 1, up=200, t=t0 + 200), jrec(31, "boot", 1, 1, 1), jrec(32, "session", 44, 3, 1),
              jrec(33, "wifi_disconnect", 201, -70, 30, t0 + 400)]
    entries = first + steps + second
    every, starts, casters, part = run_ui([
        ("journalItems", [entries, True, None]),
        ("journalItems", [entries, True, ["boot", "session", "reboot", "safe_mode", "rx_boot"]]),
        ("journalItems", [entries, True, ["caster_down", "caster_up", "caster_stop"]]),
        ("journalItems", [steps[-5:] + second, False, None]),
    ])
    heads = [i["head"][1] for i in every if "head" in i]
    assert heads == ["Restarted by you (previous run 44\u00a0s)", "Started after power-on or a reset by its pin"]
    rows = [i for i in every if "head" not in i]
    assert [r["ev"] for r in rows] == ["Wi-Fi dropped", "Recovery step"]   # the restart record went into the heading
    assert len(rows[1]["all"]) == 27 and " to " in rows[1]["when"]
    assert rows[0]["det"] == "network not found (201), signal -70 dBm"
    text = json.dumps(every, ensure_ascii=False)
    assert "≈" not in text and "#" not in text and "slot" not in text.lower()
    # Starts only: the two headings; casters only: nothing; the oldest start not loaded yet is a plain "Start".
    assert [("head" in i) for i in starts] == [True, True]
    assert casters == []
    assert [i["head"][1] for i in part if "head" in i] == ["Restarted by you (previous run 44\u00a0s)", "Start"]


def test_event_filter_covers_every_journal_kind():
    (filters,) = run_ui([("evFilter", None)])
    kinds = {k for f in filters if f[2] for k in f[2]}
    api = (ROOT / "components" / "web" / "API.md").read_text(encoding="utf-8")
    section = api[api.index("### `GET /api/journal?cursor=N`"):api.index("### `DELETE /api/journal`")]
    documented = set(re.findall(r"^\| `([a-z_]+)` \|", section, flags=re.M)) - {"kind"}
    assert documented and documented <= kinds, documented - kinds
    assert filters[0][:2] == ["all", "All events"]


def test_today_online_share_on_the_dashboard():
    """Whole numbers at 0 % and 100 % (no "0.00 %"), two decimals in between, rounded down: never "100.00 %"
    while the caster was offline for a moment."""
    out = run_ui([("onlineShare", [0, 600]), ("onlineShare", [600, 0]), ("onlineShare", [1, 2]),
                  ("onlineShare", [86399, 1])])
    assert out == ["0\u00a0%", "100\u00a0%", "33.33\u00a0%", "99.99\u00a0%"], out


# ---- The station's state (stationState): the Status page's top card, Onocoy's day, the notices it covers ----

def caster(slot: int, name: str, state: str, **kw) -> dict:
    c = {"slot": slot, "name": name, "empty": False, "enabled": state != "disabled", "protected": name == "Onocoy",
         "host": "servers.onocoy.com" if name == "Onocoy" else name.lower() + ".example", "state": state, "reason": "none",
         "next_retry_s": 0, "stop_reason": "none", "onocoy_ready": name == "Onocoy"}
    if name == "Onocoy":
        c.update({"streak_ok": True, "budget_s": 473, "budget_left_s": 463, "today_online_s": 3600, "today_offline_s": 10,
                  "data_rate_permille": 1000, "connected_s": 3600})
    c.update(kw)
    return c


def status(**kw) -> dict:
    """An admin GET /api/status: the receiver streams 4 bands with GLONASS, Onocoy streams, nothing is wrong."""
    s = {"api": 1, "public": False, "uptime_s": 23520,
         "device": {"hostname": "onocoydiy", "settings_available": True},
         "safe_mode": {"active": False, "retry_in_s": None, "crashes": 0},
         "network": {"online": True, "ssid": "Home"},
         "receiver": {"state": "streaming", "model": "UM982", "rtcm_age_ms": 180, "journal": "none", "uart_closed": False,
                      "rx_maintenance": False, "maintenance_left_s": 0, "first_apply_held": False, "best_sys": "galileo",
                      "best_bands": 4, "glonass": True, "rtcm1230_supported": True, "position_locked": True,
                      "profile_applied": True, "signal_test": None},
         "casters": [caster(0, "Onocoy", "streaming")],
         "health": {"level": "ok", "last_step": "none", "recovery_mode": "full"},
         "alerts": [], "availability": {"protected_down_today_s": 10, "budget_left_s": 463}, "update": {}}
    for k, v in kw.items():
        if isinstance(v, dict) and isinstance(s.get(k), dict):
            s[k] = {**s[k], **v}
        else:
            s[k] = v
    return s


def heroes(*statuses, admin: bool = True) -> list[dict]:
    return run_ui([("hero", [st, admin]) for st in statuses])


NO_RX = {"id": "no_receiver", "source": "station", "saw_bytes": False, "next_detect_in_s": 24}


def test_state_every_condition_in_order():
    """Each condition of the list, alone: its key, title and colour. Then each next to the one after it: the
    earlier one wins (what stops everything first, Onocoy's quality last)."""
    conds = {
        "safe_mode": dict(safe_mode={"active": True, "crashes": 3, "retry_in_s": 2690}, receiver={"state": "idle"},
                          casters=[caster(0, "Onocoy", "disabled")], alerts=[{"id": "safe_mode", "source": "health"}]),
        "journal": dict(receiver={"journal": "waiting"}),
        "settings": dict(device={"settings_available": False}, alerts=[{"id": "settings_degraded", "source": "health"}]),
        "updating": dict(receiver={"state": "flashing", "journal": "running"}),
        "uart_closed": dict(receiver={"state": "no_receiver", "uart_closed": True}, alerts=[NO_RX]),
        "no_receiver": dict(receiver={"state": "no_receiver", "rtcm_age_ms": None}, alerts=[NO_RX],
                            health={"level": "recovering", "last_step": "rescan"}),
        "rx_stopped": dict(receiver={"rtcm_age_ms": 30000}, health={"level": "recovering", "last_step": "link_restart"}),
        "no_satellites": dict(health={"level": "failed"}, alerts=[{"id": "no_satellites", "source": "health"}],
                              casters=[caster(0, "Onocoy", "wait_data")]),
        "no_rtcm": dict(health={"level": "failed"}, alerts=[{"id": "no_rtcm", "source": "health"}],
                        casters=[caster(0, "Onocoy", "wait_data")]),
        "stopped": dict(casters=[caster(0, "Onocoy", "stopped", stop_reason="duplicate")]),
        "maintenance": dict(receiver={"state": "idle", "rx_maintenance": True, "maintenance_left_s": 830}),
        "signal_test": dict(receiver={"state": "configuring", "signal_test": {"state": "measuring", "phase": 0, "left_s": 600,
                                                                              "group": {"master": 3, "slave": 6}}}),
        "wifi_trial": dict(network={"cred_trial": True, "cred_trial_left_s": 80, "pending_ssid": "Cabin"}),
        "receiver": dict(receiver={"state": "configuring"}, casters=[caster(0, "Onocoy", "wait_data")]),
        "onocoy_setup": dict(casters=[caster(0, "Onocoy", "disabled", onocoy_ready=False), caster(1, "RTK2go", "backoff")]),
        "casters_down": dict(casters=[caster(0, "Onocoy", "streaming"), caster(1, "Bench", "auth_failed", reason="auth")],
                             receiver={"best_bands": 3}),
        "quality": dict(receiver={"best_bands": 3}),
        "streaming": dict(),
    }
    expect = {
        "safe_mode": ("Safe mode", "bad"), "journal": ("Receiver update needs you", "warn"),
        "settings": ("Settings cannot be saved", "bad"), "updating": ("Updating the receiver", "info"),
        "uart_closed": ("Receiver port closed", "bad"), "no_receiver": ("No receiver found", "bad"),
        "rx_stopped": ("Receiver stopped sending data", "warn"), "no_satellites": ("Antenna sees no satellites", "bad"),
        "no_rtcm": ("Receiver sends no corrections", "bad"), "stopped": ("Onocoy stopped", "bad"),
        "maintenance": ("Receiver maintenance", "info"), "signal_test": ("Comparing signal groups", "info"),
        "wifi_trial": ("Trying new Wi-Fi", "info"), "receiver": ("Configuring the receiver", "info"),
        "onocoy_setup": ("Onocoy not set up", "warn"), "casters_down": ("Partly streaming", "warn"),
        "quality": ("Too few bands for Onocoy", "warn"), "streaming": ("Streaming", "ok"),
    }
    keys = list(conds)
    out = heroes(*[status(**conds[k]) for k in keys])
    for k, o in zip(keys, out):
        assert (o["key"], o["title"], o["tone"]) == (k, *expect[k]), (k, o)
        text = (o["title"] + " " + o["line"]).lower()
        # No health level, no setting name and no internal state on top, ever.
        assert not any(w in text for w in ("alerts only", "alerts_only", "needs attention", "healthy", "recovery:",
                                           "degraded", "saveconfig", "rtcm")), (k, o)
    # Two conditions at once: the earlier one in the list wins.
    pairs = []
    for a, b in zip(keys, keys[1:]):
        merged = {}
        for part in (conds[b], conds[a]):
            for kk, vv in part.items():
                if kk == "alerts":
                    merged["alerts"] = merged.get("alerts", []) + vv
                elif isinstance(vv, dict) and isinstance(merged.get(kk), dict):
                    merged[kk] = {**merged[kk], **vv}
                else:
                    merged[kk] = vv
        pairs.append((a, b, status(**merged)))
    got = heroes(*[p[2] for p in pairs])
    for (a, b, _), o in zip(pairs, got):
        assert o["key"] == a, (a, b, o)


def test_hero_says_what_blocks_first_and_what_to_do():
    onocoy_not_set_up = status(casters=[caster(0, "Onocoy", "disabled", onocoy_ready=False)],
                               health={"level": "degraded", "recovery_mode": "alerts_only"})
    maintenance = status(receiver={"state": "idle", "rx_maintenance": True, "maintenance_left_s": 830})
    # The QEMU run: no receiver found while the recovery still reports recovering.
    no_rx = status(receiver={"state": "no_receiver", "rtcm_age_ms": None}, health={"level": "recovering", "last_step": "rescan"},
                   alerts=[NO_RX], casters=[caster(0, "Onocoy", "wait_data")])
    stall = status(receiver={"rtcm_age_ms": 30000}, health={"level": "recovering", "last_step": "link_restart"})
    alerts_only = status(health={"level": "degraded", "recovery_mode": "alerts_only"},
                         alerts=[{"id": "no_rtcm", "source": "health"}], casters=[caster(0, "Onocoy", "wait_data")])
    silent_alerts_only = status(health={"level": "degraded", "recovery_mode": "alerts_only"},
                                alerts=[{"id": "receiver_silent", "source": "health"}], casters=[caster(0, "Onocoy", "wait_data")])
    out = heroes(onocoy_not_set_up, maintenance, no_rx, stall, alerts_only, silent_alerts_only)

    assert out[0]["title"] == "Onocoy not set up" and out[0]["tone"] == "warn", out[0]
    assert out[0]["line"] == "Add your Onocoy credentials to start streaming."
    assert out[0]["action"] == "Set up Onocoy" and out[0]["primary"] is True
    # The recovery choice shows as a fact only when it is not the default.
    assert out[0]["facts"] == [["If corrections stop", "Only tell me"]]

    assert out[1]["title"] == "Receiver maintenance"
    assert out[1]["line"] == "The station sends the receiver no commands. Ends in about 14 min."
    assert out[1]["action"] == "end-maint" and out[1]["notice"] == "maintenance"

    assert out[2]["title"] == "No receiver found" and out[2]["tone"] == "bad", out[2]
    assert out[2]["line"].startswith("Nothing arrives from the receiver. Check its power") and "Next search in 24 s." in out[2]["line"]
    assert out[2]["action"] == "detect" and out[2]["alert"] == "no_receiver" and out[2]["also"] == []

    # A stall while the station still says streaming: never "Streaming" on top, and Onocoy does not count as streaming.
    assert out[3]["title"] == "Receiver stopped sending data" and out[3]["tone"] == "warn", out[3]
    assert out[3]["line"] == "The station is fixing it: reconnecting to the receiver."
    assert out[3]["ono"] is False and out[3]["delivering"] is False

    assert out[4]["title"] == "Receiver sends no corrections" and out[4]["alert"] == "no_rtcm", out[4]
    assert out[4]["line"] == ("The receiver answers but sends no corrections. Check that the antenna is on the socket marked M "
                              "and sees the open sky. Automatic recovery is off (your choice).")
    assert out[5]["title"] == "Receiver stopped sending data" and out[5]["tone"] == "bad"
    assert out[5]["line"].endswith("Automatic recovery is off (your choice).")
    assert out[5]["facts"] == [["If corrections stop", "Only tell me"]]


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
    assert out[0]["facts"] == [["Casters", "1 of 2 streaming"]]
    assert out[1]["title"] == "Not streaming" and out[1]["tone"] == "bad"
    assert out[1]["line"] == "Onocoy and Bench are not streaming."
    assert out[2]["line"] == "3 casters are not streaming."
    assert out[3] == {**out[3], "title": "Streaming", "tone": "ok", "line": "Onocoy is receiving corrections.", "action": None}
    # One caster turned on: no "1 of 1" fact.
    assert out[3]["facts"] == []
    assert out[4]["line"] == "Onocoy and RTK2go are receiving corrections."
    # The line names both as streaming: the count would say it again.
    assert out[4]["facts"] == []
    assert out[5]["line"] == "Waiting for corrections from the receiver."
    assert out[6]["line"] == "Bench is not streaming: DNS lookup failed."
    # The public form: states only, no reason, no button.
    pub = heroes({"api": 1, "public": True, "uptime_s": 60, "device": {"hostname": "onocoydiy"},
                  "receiver": {"state": "streaming", "best_bands": 4},
                  "casters": [{"slot": 0, "name": "Onocoy", "state": "streaming"}, {"slot": 1, "name": "Bench", "state": "backoff"}],
                  "health": {"level": "ok"}}, {"api": 1, "public": True, "uptime_s": 60, "device": {"hostname": "onocoydiy"},
                  "receiver": {"state": "no_receiver", "best_bands": 0}, "casters": [{"slot": 0, "name": "Onocoy", "state": "wait_data"}],
                  "health": {"level": "recovering"}}, admin=False)
    assert pub[0]["line"] == "Bench is not streaming." and pub[0]["action"] is None and pub[0]["title"] == "Partly streaming"
    assert pub[1]["title"] == "No receiver found" and pub[1]["line"] == "Nothing arrives from the receiver." and pub[1]["actions"] == []


def test_hero_safe_mode_stop_and_alerts_while_streaming():
    safe = status(safe_mode={"active": True, "crashes": 3, "retry_in_s": 2690}, receiver={"state": "idle"},
                  casters=[caster(0, "Onocoy", "disabled")], alerts=[{"id": "safe_mode", "source": "health"}])
    stopped = status(casters=[caster(0, "Onocoy", "stopped", stop_reason="duplicate")])
    link_loss = status(alerts=[{"id": "link_loss", "source": "health"}, {"id": "power_supply", "source": "health"},
                               {"id": "heap_fragmented", "source": "health"}])
    bare_level = status(health={"level": "degraded"})
    off = status(casters=[caster(0, "Onocoy", "disabled", enabled=False), caster(1, "RTK2go", "disabled", enabled=False, onocoy_ready=False)])
    none = status(casters=[])
    out = heroes(safe, stopped, link_loss, bare_level, off, none)
    assert out[0]["title"] == "Safe mode" and out[0]["tone"] == "bad"
    assert out[0]["line"] == "The receiver and casters are off after 3 crashes. The station tries normal operation again in 45 min."
    assert out[0]["alert"] == "safe_mode" and out[0]["notice"] == "safe_mode" and out[0]["also"] == []
    # Two buttons in safe mode: the cure (an update) and leaving it, a button the page handles.
    assert out[0]["actions"] == ["Updates", "leave-safe"]
    assert out[1]["line"] == "Duplicate stream. The station makes no new attempt until you act."
    assert out[1]["action"] == "What to do" and out[1]["notice"] == "onocoy_stopped:0"
    # Other alerts while everything streams: "Also:" lines with their own words; the title stays and so does its colour.
    assert out[2]["title"] == "Streaming" and out[2]["tone"] == "ok" and out[2]["line"] == "Onocoy is receiving corrections."
    assert out[2]["also"] == ["link_loss", "power_supply", "heap_fragmented"]
    assert out[2]["alsoText"][0].startswith("Receiver data lost. Some data from the receiver was lost")
    # The level alone (no alert to show) leaves the dot green and adds nothing.
    assert out[3]["tone"] == "ok" and out[3]["line"] == "Onocoy is receiving corrections." and out[3]["also"] == []
    assert out[4]["line"] == "Onocoy is set up but turned off. Turn it on to start streaming." and out[4]["action"] == "Turn Onocoy on"
    assert out[5]["line"] == "Add a caster to start streaming." and out[5]["action"] == "Add a caster"


def test_hero_quality_minimums_for_onocoy():
    """Streaming to Onocoy, but a minimum this board can miss is missed: 4 bands, GLONASS, message 1230. Only while
    the receiver delivers, and unknown values (an older firmware, never configured) flag nothing."""
    out = heroes(status(receiver={"best_bands": 3}), status(receiver={"glonass": False}),
                 status(receiver={"rtcm1230_supported": False}), status(receiver={"best_bands": 3, "glonass": False}),
                 status(receiver={"rtcm1230_supported": None}),
                 status(receiver={"glonass": None, "rtcm1230_supported": None}),
                 status(receiver={"best_bands": 0}),
                 status(receiver={"best_bands": 3}, alerts=[{"id": "bands", "source": "health"}]),
                 status(receiver={"best_bands": 3}, casters=[caster(0, "Onocoy", "disabled", enabled=False),
                                                              caster(1, "RTK2go", "streaming", onocoy_ready=False)]))
    assert out[0]["line"] == "Onocoy gets 3 bands; its quality score needs 4 on one constellation." and out[0]["action"] == "Receiver"
    assert out[1]["title"] == "No GLONASS for Onocoy" and out[1]["line"] == "Onocoy gets no GLONASS; its quality score needs it."
    assert out[2]["title"] == "No GLONASS for Onocoy" and "message 1230" in out[2]["line"]
    assert out[3]["title"] == "Too few bands for Onocoy" and "3 bands and no GLONASS" in out[3]["line"]
    assert [o["key"] for o in out[4:7]] == ["streaming", "streaming", "streaming"]
    # The bands alert is the line: not an "Also:" line too.
    assert out[7]["key"] == "quality" and out[7]["alert"] == "bands" and out[7]["also"] == []
    # Onocoy off: its quality does not matter.
    assert out[8]["key"] == "streaming" and out[8]["line"] == "RTK2go is receiving corrections."


def test_onocoy_today_verdict_from_one_rule():
    """Missed while streak_ok is false; At risk while Onocoy does not stream now, more than 60 % of the offline time is
    used, the data rate is under 98 % after 10 minutes connected, or a quality minimum is missed; On track otherwise.
    Never "On track" while the top card says Onocoy is down or the receiver stalls."""
    ono = lambda **kw: [caster(0, "Onocoy", **{"state": "streaming", **kw})]
    cases = [
        status(),
        status(casters=ono(streak_ok=False, budget_left_s=0), availability={"protected_down_today_s": 500, "budget_left_s": 0}),
        status(casters=ono(streak_ok=False, budget_s=0, budget_left_s=0), availability={"protected_down_today_s": 0, "budget_left_s": 0}),
        status(casters=ono(state="backoff", reason="timeout")),
        status(casters=ono(state="stopped", stop_reason="in_use")),
        status(receiver={"rtcm_age_ms": 30000}, health={"level": "recovering", "last_step": "resync"}),
        status(availability={"protected_down_today_s": 300, "budget_left_s": 173}),
        status(casters=ono(data_rate_permille=970, connected_s=900)),
        status(casters=ono(data_rate_permille=971, connected_s=34)),
        status(receiver={"best_bands": 3}),
        status(receiver={"glonass": False}, casters=ono() + [caster(1, "Bench", "auth_failed", reason="auth")]),
        status(casters=[caster(0, "Onocoy", "disabled", onocoy_ready=False)]),
        status(availability={"protected_down_today_s": 10, "budget_left_s": None}),
    ]
    out = run_ui([("onocoy", c) for c in cases])
    assert out[0] == {**out[0], "verdict": "On track", "tone": "ok", "why": None, "left": 463}
    assert out[1]["verdict"] == "Missed" and out[1]["tone"] == "bad" and out[1]["why"] == "Today's offline time is over the budget."
    assert out[2]["verdict"] == "Missed" and "epochs were lost" in out[2]["why"]
    assert out[3]["verdict"] == "At risk" and out[3]["why"] == "Offline time is counting now."
    assert out[4]["verdict"] == "At risk" and out[4]["hero"] == "stopped"
    assert out[5]["verdict"] == "At risk" and out[5]["hero"] == "rx_stopped"
    assert out[6]["verdict"] == "At risk" and out[6]["why"] == "Most of today's offline time is used."
    # The data rate shows only as the cause, and only after 10 minutes connected.
    assert out[7]["verdict"] == "At risk" and out[7]["why"].startswith("Data rate 97.0 %")
    assert out[8]["verdict"] == "On track" and out[8]["why"] is None
    # The top card says the quality problem: the verdict does not repeat it.
    assert out[9]["verdict"] == "At risk" and out[9]["hero"] == "quality" and out[9]["why"] is None
    assert out[10]["verdict"] == "At risk" and out[10]["hero"] == "casters_down"
    assert out[10]["why"] == "No GLONASS: Onocoy's quality score needs it."
    # Onocoy not set up: no card (the top card has the setup button).
    assert out[11] is None
    # Without a trusted clock the offline time left is unknown; the verdict still holds.
    assert out[12]["verdict"] == "On track" and out[12]["left"] is None


def test_state_during_maintenance_restarts_and_one_caster_down():
    """Receiver maintenance: the station says idle while the corrections keep coming, so Onocoy still streams and its
    day stays on track. The restart plan shows while the receiver is fixed or looked for (full recovery only). A line
    about one caster names it as its subject (its row on the page does not say why again)."""
    maint = status(receiver={"state": "idle", "rx_maintenance": True, "maintenance_left_s": 600, "rtcm_age_ms": 300})
    maint_quiet = status(receiver={"state": "idle", "rx_maintenance": True, "maintenance_left_s": 600, "rtcm_age_ms": 9000},
                         casters=[caster(0, "Onocoy", "wait_data")])
    hold = {"reboot_hold": ["admin"], "reboot_hold_left_s": 1578, "reboot_cap_wait_s": None, "reboot_cap_limit": None}
    stall = status(receiver={"rtcm_age_ms": 30000}, health={"level": "recovering", "last_step": "link_restart", **hold})
    stall_alerts_only = status(receiver={"rtcm_age_ms": 30000}, health={"level": "recovering", "recovery_mode": "alerts_only", **hold})
    one_down = status(casters=[caster(0, "Onocoy", "streaming"), caster(2, "Bench", "auth_failed", reason="auth")])
    out = heroes(maint, maint_quiet, stall, stall_alerts_only, one_down, status())
    assert out[0]["key"] == "maintenance" and out[0]["delivering"] is True and out[0]["ono"] is True
    assert out[1]["delivering"] is False
    assert out[2]["plan"] is not None and "restarts itself in 27\u00a0min at the latest" in out[2]["plan"]
    assert out[3]["plan"] is None
    assert out[4]["subject"] == 2 and out[5]["plan"] is None and out[5]["subject"] is None
    days = run_ui([("onocoy", maint), ("onocoy", maint_quiet)])
    assert days[0]["verdict"] == "On track" and days[1]["verdict"] == "At risk"


def test_status_page_leaves_out_the_notice_the_top_card_says():
    maint = status(receiver={"state": "idle", "rx_maintenance": True, "maintenance_left_s": 600},
                   update={"app_available": True, "app_version": "v0.4.0"})
    stopped = status(casters=[caster(0, "Onocoy", "stopped", stop_reason="duplicate")])
    settings = status(device={"settings_available": False}, alerts=[{"id": "settings_degraded", "source": "health"}])
    # Settings storage goes before the casters: the top card says it even while a caster is down.
    settings_down = status(device={"settings_available": False}, alerts=[{"id": "settings_degraded", "source": "health"}],
                           casters=[caster(0, "Onocoy", "backoff", reason="timeout")])
    # A Wi-Fi trial is the top card's while nothing more important holds.
    trial = status(network={"cred_trial": True, "cred_trial_left_s": 80, "pending_ssid": "Cabin"})
    trial_stopped = status(network={"cred_trial": True, "cred_trial_left_s": 80, "pending_ssid": "Cabin"},
                           casters=[caster(0, "Onocoy", "stopped", stop_reason="duplicate")])
    out = run_ui([("notices", [maint, ""]), ("notices", [maint, "casters"]), ("notices", [stopped, ""]),
                  ("notices", [stopped, "receiver"]), ("notices", [settings, ""]), ("notices", [settings, "diag"]),
                  ("notices", [settings_down, ""]), ("notices", [trial, ""]), ("notices", [trial_stopped, ""])])
    keys = [[k for k, _ in o] for o in out]
    assert keys[0] == ["update"], out[0]
    assert keys[1] == ["maintenance", "update"]
    assert keys[2] == [] and keys[3] == ["onocoy_stopped:0"]
    assert out[3][0][1] == "Onocoy stopped: duplicate stream"
    assert keys[4] == [] and keys[5] == ["settings"] and keys[6] == []
    assert out[0][0][1] == "Update available"
    assert keys[7] == [] and keys[8] == ["wifi"]
    (t,) = heroes(trial)
    assert t["trial"] == "Cabin" and t["line"].startswith("If it gets no address there")


def test_band_wording_never_promises_a_top_reward():
    """4 bands is the most this receiver reaches (the fifth is Galileo E5 AltBOC, which the UM982 lacks): no page
    calls it Onocoy's top band reward."""
    for f in UI_JS.glob("*.js"):
        assert "top band reward" not in f.read_text(), f.name
    four = {"totals": {"tracked": 30, "cnr_dbhz": 44, "msm_age_ms": 100,
                       "systems": [{"sys": "galileo", "bands": {"l1": 8, "l5": 8, "l6": 7, "l2": 8}}]}}
    three = {"totals": {"tracked": 30, "cnr_dbhz": 44, "msm_age_ms": 100,
                        "systems": [{"sys": "gps", "bands": {"l1": 8, "l2": 8, "l5": 3}}]}}
    a, b = run_ui([("checks", four), ("checks", three)])
    assert a["items"][2][2] == "4 bands on one constellation: the most this receiver can reach."
    assert b["items"][2][2].startswith("Onocoy's quality score needs 4 bands on one constellation.")


def test_password_mismatch_and_unknown_signal():
    mismatch, short, ok = run_ui([("pwProblem", ["abcdefgh", "abcdefgi"]), ("pwProblem", ["abc", "abc"]),
                                  ("pwProblem", ["abcdefgh", "abcdefgh"])])
    # The setup step and the System page pick the repeat field by this exact text.
    assert mismatch == "The passwords do not match."
    assert short == "Use at least 8 characters." and ok is None
    assert run_ui([("rssi", 0), ("rssi", None)]) == ["–", "–"]


# ---- The receiver pages (60-receiver.js, 05-labels.js) ----

def test_signal_groups_short_and_plain():
    (choices,) = run_ui([("sgChoices", None)])
    names = run_ui([("sgName", [int(c[0].split()[0]), int(c[0].split()[1])]) for c in choices] + [("sgName", [9, 9])])
    assert names == ["3 6 (Galileo E6)", "7 0 (BeiDou B2a)", "4 5 (factory)", "5 0 (no L5)", "9 9"]
    for pair, name, line, short in choices:
        # One short line each; "(recommended)" is the Receiver page's mark, never part of the name others reuse.
        assert line.endswith(".") and len(line) <= 60, pair
        assert "recommended" not in (name + line + short).lower(), pair
        assert not any(m in line for m in ("25164", "build", "2026")), pair


def test_receiver_management_one_choice_over_two_settings():
    """Receiver settings: one choice for automatic configuration and what happens if corrections stop. Each option one
    concrete line; any other combination is Custom, says what it does and offers the option nearest to it."""
    (m,) = run_ui([("manage", None)])
    assert {k: v[:3] for k, v in m.items()} == {
        "station": [True, "full", "The station (recommended)"],
        "no_restart": [True, "no_reboot", "The station, but never restart itself"],
        "me": [False, "alerts_only", "Me (alerts only)"]}
    for _, _, name, line in m.values():
        assert line.endswith(".") and len(line) <= 140 and not any(w in line for w in ("RTCM", "SAVECONFIG", "autocfg"))
    assert "restarts itself" in m["station"][3] and "never restarts" in m["no_restart"][3]
    assert m["me"][3].startswith("The station sends the receiver nothing")
    keys = run_ui([("manageKey", [a, r]) for a in (True, False) for r in ("full", "no_reboot", "alerts_only")])
    assert keys == ["station", "no_restart", None, None, None, "me"]
    custom = run_ui([("manageCustom", [True, "alerts_only"]), ("manageCustom", [False, "full"]),
                     ("manageCustom", [False, "no_reboot"])])
    assert custom[0] == ["The station configures the receiver but does nothing if corrections stop.", "station"]
    assert custom[1][1] == custom[2][1] == "me"
    assert custom[1][0].endswith("still resets it and may restart itself.") and custom[2][0].endswith("still resets it.")
    # The Status page and Diagnostics name the recovery choice alone, without "(recommended)".
    assert run_ui([("recoveryLbl", "full"), ("recoveryLbl", "alerts_only")]) == ["Fix it automatically", "Only tell me"]


def test_receiver_chip_never_says_streaming_while_corrections_stopped():
    """The Receiver card's chip comes from the same state as the Status page's top card."""
    stall = status(receiver={"rtcm_age_ms": 30000}, health={"level": "recovering", "last_step": "link_restart"})
    silent = status(alerts=[{"id": "receiver_silent", "source": "health"}], health={"level": "failed"})
    no_rx = status(receiver={"state": "no_receiver"}, alerts=[NO_RX], casters=[caster(0, "Onocoy", "wait_data")])
    maint = status(receiver={"state": "idle", "rx_maintenance": True, "maintenance_left_s": 600, "rtcm_age_ms": 300})
    detect = status(receiver={"state": "detecting"}, casters=[caster(0, "Onocoy", "wait_data")])
    stopped = status(casters=[caster(0, "Onocoy", "stopped", stop_reason="duplicate")])
    out = run_ui([("rxChip", x) for x in (status(), stall, silent, no_rx, maint, detect, stopped)])
    assert out == [["Streaming", "ok"], ["Stopped sending data", "warn"], ["Stopped sending data", "bad"],
                   ["No receiver found", "bad"], ["Maintenance", "info"], ["Looking for the receiver", "info"],
                   ["Streaming", "ok"]]


def test_survey_pauses_without_data_on_both_pages():
    surveying = {"position": {"locked": False, "source": "survey"}, "survey": {"active": True, "window_left_s": 660},
                 "profile": {"base_mode": "survey_in"}}
    locked = {"position": {"locked": True, "source": "survey"}, "survey": {"active": False}, "profile": {"base_mode": "fixed",
              "fixed": {"lat_deg": 1, "lon_deg": 2, "height_m": 3}}}
    typed = {"position": {"locked": False, "source": "none"}, "profile": {"base_mode": "fixed", "fixed": {"lat_deg": 1}}}
    out = run_ui([("posLine", [surveying, True]), ("posLine", [surveying, False]), ("posLine", [locked, False]),
                  ("posLine", [typed, True]), ("posLine", [{"position": {"source": "none"}}, True]),
                  ("statusPos", [{"surveying": True, "survey_window_left_s": 660}, True]),
                  ("statusPos", [{"surveying": True, "survey_window_left_s": 660}, False])])
    assert out[0] == "Surveying, about 11\u00a0min left. Then lock it."
    assert out[1] == "Surveying, paused while no corrections arrive."
    assert out[2].startswith("Locked.") and out[3].startswith("Fixed (typed in).") and out[4] == "No position yet."
    assert out[5] == "Surveying, 11\u00a0min left" and out[6] == "Surveying, paused"


SIG = {"best_sys": "galileo", "best_bands": 4, "e6_lost": False, "epoch_age_ms": 420, "rtcm1230_supported": True,
       "systems": [{"sys": "gps", "satellites": 11, "bands": ["l1", "l2", "l5"], "cnr_dbhz": {"l1": 45, "l2": 41, "l5": 44}},
                   {"sys": "glonass", "satellites": 9, "bands": ["l1", "l2"], "cnr_dbhz": {"l1": 45, "l2": 43}},
                   {"sys": "galileo", "satellites": 11, "bands": ["l1", "l2", "l5", "l6"],
                    "cnr_dbhz": {"l1": 41, "l2": 41, "l5": 41, "l6": 38}},
                   {"sys": "qzss", "satellites": 1, "bands": ["l1", "l2", "l5", "l6"], "cnr_dbhz": {"l1": 40}}]}


def test_signals_card_headline_and_hints():
    """The Signals card: the best constellation while observations arrive, how long they have been missing when they
    stop (never "No signals yet" after a stall). The hints are the antenna check's, from the signal summary: QZSS never
    counts as the best; E6 lost, no GLONASS and a refused 1230 are added when they apply."""
    stale = {**SIG, "epoch_age_ms": 37000}
    out = run_ui([("headline", SIG), ("headline", stale), ("headline", {"epoch_age_ms": None, "systems": []})])
    assert out == ["Galileo · 4 bands", "No signals for 37\u00a0s", "No signals yet"]
    no_e6 = {**SIG, "best_bands": 3, "systems": [s if s["sys"] != "galileo" else {**s, "bands": ["l1", "l2", "l5"]}
                                                 for s in SIG["systems"] if s["sys"] != "qzss"]}
    lost = {**SIG, "e6_lost": True}
    no_glo = {**SIG, "systems": [s for s in SIG["systems"] if s["sys"] != "glonass"]}
    refused = {**SIG, "rtcm1230_supported": False}
    good, bad3, e6, glo, r1230 = run_ui([("sigChecks", x) for x in (SIG, no_e6, lost, no_glo, refused)])
    assert [i[0] for i in good["items"]] == ["ok", "ok", "ok"] and good["ok"] is True
    assert good["items"][0][1] == "32 satellites tracked" and good["items"][2][1] == "Galileo: 4 bands"
    assert bad3["items"][2][0] == "warn" and bad3["items"][2][2].endswith("Signal group 3 6 adds Galileo E6.")
    assert e6["items"][-1] == ["bad", "Galileo E6 lost", "It was tracked before with this signal group."] and e6["ok"] is False
    assert glo["items"][-1][:2] == ["warn", "No GLONASS"]
    assert r1230["items"][-1][:2] == ["warn", "GLONASS biases refused"]


def test_position_message_waits_for_the_survey():
    """While the receiver surveys it sends no position (1005): that row waits, it is not red."""
    never = {"msg": 1005, "count": 0, "age_ms": None, "expected_period_s": 1}
    ok = {"msg": 1077, "count": 900, "age_ms": 400, "expected_period_s": 1}
    late = {"msg": 1077, "count": 900, "age_ms": 9000, "expected_period_s": 1}
    out = run_ui([("msgCheck", [never, True, True]), ("msgCheck", [never, True, False]), ("msgCheck", [ok, True, True]),
                  ("msgCheck", [late, True, True]), ("msgCheck", [late, False, False])])
    assert out == ["waits", "missing", "", "missing", ""]


def test_message_and_satellite_lists_are_one_line_rows():
    """No stacked blocks for the RTCM messages and the satellites on a phone: one line per row."""
    for name in ("65-signals.js", "66-live.js"):
        text = (UI_JS / name).read_text()
        assert "stack-sm" not in text and "stack: true" not in text, name
        assert "class: 'rows'" in text, name
    css = (UI_JS.parent / "app.css").read_text()
    assert "table.rows th, table.rows td { padding: 0.35rem 0.45rem; white-space: nowrap; }" in css


def test_firmware_build_shown_once_and_distances_in_mm_and_cm():
    out = run_ui([("fwText", ["R4.10Build25164", 25164]), ("fwText", ["R4.10", 25164]), ("fwText", [None, 25164]),
                  ("dist", 0.0021), ("dist", 0.0), ("dist", 0.023), ("dist", 3.5), ("dist", None)])
    assert out[:3] == ["R4.10Build25164", "R4.10 (build 25164)", "\u2013"]
    assert out[3:] == ["2\u00a0mm", "0\u00a0mm", "2.3\u00a0cm", "3.50\u00a0m", "\u2013"]


def test_comparison_says_why_it_cannot_start_in_the_stations_order():
    ok = {"state": "streaming", "autocfg": True, "first_apply_held": False, "maintenance": {"on": False},
          "position": {"locked": True}}
    out = run_ui([("sgtBlock", {**ok, "first_apply_held": True, "autocfg": False}),
                  ("sgtBlock", {**ok, "autocfg": False}),
                  ("sgtBlock", {**ok, "maintenance": {"on": True}}),
                  ("sgtBlock", {**ok, "state": "no_receiver", "position": {"locked": False}}),
                  ("sgtBlock", {**ok, "position": {"locked": False}}),
                  ("sgtBlock", ok)])
    assert out == ["Configure the receiver first.", "Needs automatic configuration on.", "Not during receiver maintenance.",
                   "Needs a connected receiver.", "Lock the base position first.", ""]


def test_comparison_lines_short_and_in_hours():
    measuring = {"state": "measuring", "phase": 0, "left_s": 1490, "running_group": {"master": 3, "slave": 6}}
    switching = {"state": "switching", "phase": 1, "left_s": 0, "running_group": {"master": 7, "slave": 0}}
    st = status(receiver={"signal_test": {"state": "deciding", "decide_left_s": 86000, "group": {"master": 7, "slave": 0}}})
    line, sw, idle, notice = run_ui([("sgtLine", measuring), ("sgtLine", switching), ("sgtLine", {"state": "idle"}),
                                     ("sgtNotice", st)])
    assert line == "Group 1 of 2, 3 6 (Galileo E6): measuring, about 25\u00a0min left."
    assert sw == "Switching the receiver to 7 0 (BeiDou B2a). Corrections pause briefly."
    assert idle is None
    # The notice says what the top card says, in one line.
    assert notice[1] == "Choose a signal group"
    assert notice[2] == "With no choice the old group comes back in about 24\u00a0h." and " 1 d" not in notice[2]
    # Elsewhere a running comparison is one title and a few words.
    running = status(receiver={"signal_test": {"state": "measuring", "phase": 1, "left_s": 300, "group": {"master": 7, "slave": 0}}})
    switching = status(receiver={"signal_test": {"state": "switching", "phase": 0, "left_s": 0, "group": {"master": 3, "slave": 6}}})
    n, sw = run_ui([("sgtNotice", running), ("sgtNotice", switching)])
    assert n[1] == "Comparing signal groups"
    assert n[2] == "Group 2 of 2, about 5\u00a0min left."
    assert sw[2] == "Corrections pause briefly."


def test_profile_chip_and_maintenance_notice_on_the_receiver_page():
    chips = run_ui([("profileChip", {"state": "configuring", "profile_applied": True}),
                    ("profileChip", {"state": "streaming", "config_failed": True}),
                    ("profileChip", {"state": "streaming", "profile_applied": True}),
                    ("profileChip", {"state": "streaming", "profile_applied": True, "first_apply_held": True}),
                    ("profileChip", {"state": "no_receiver", "profile_applied": False})])
    assert chips == [["Applying", "info"], ["Failed", "bad"], ["Applied", "ok"], ["Not applied", "warn"], ["Not applied", "warn"]]
    maint = status(receiver={"state": "idle", "rx_maintenance": True, "maintenance_left_s": 600})
    out = run_ui([("notices", [maint, p]) for p in ("receiver/settings", "receiver", "receiver/live", "casters", "settings", "diag")])
    # Receiver settings has the maintenance card with its buttons: no notice that repeats it there. Every other page has it.
    assert [[k for k, _ in o] for o in out] == [[]] + [["maintenance"]] * 5


# ---- The system pages (90-system.js, 30-auth.js) ----

def test_system_wifi_chip_and_recovery_line():
    out = run_ui([("wifiChip", {"configured": False}), ("wifiChip", {"configured": True, "online": True}),
                  ("wifiChip", {"configured": True, "associated": True}), ("wifiChip", {"configured": True}),
                  ("recoveryHint", None)])
    assert out[:4] == [["Not set up", "plain"], ["Online", "ok"], ["Connecting", "warn"], ["Offline", "bad"]]
    # One line under the admin password: what to do, no paragraph.
    assert out[4].startswith("Forgot it? Hold the BOOT button for 10 seconds") and "http://192.168.4.1/" in out[4]
    assert len(out[4]) < 160


def test_no_top_level_name_is_declared_twice():
    """The modules share one scope in the bundle: a second top-level function of the same name silently replaces
    the first everywhere (a page's own link helper once dropped the caster box's "↗")."""
    seen: dict[str, str] = {}
    dup = []
    for f in sorted(UI_JS.glob("*.js")):
        for n, line in enumerate(f.read_text().splitlines(), 1):
            m = re.match(r"(?:async\s+)?function\s+(\w+)|(?:const|let|var|class)\s+(\w+)\b", line)
            if not m:
                continue
            name = m.group(1) or m.group(2)
            where = f"{f.name}:{n}"
            if name in seen:
                dup.append((name, seen[name], where))
            seen.setdefault(name, where)
    assert dup == [], dup


# ---- The shell: four tabs, old addresses, notices (20-shell.js) ----

def test_four_tabs_and_old_addresses_land_on_the_new_pages():
    out = run_ui([("nav", None)] + [("moved", a) for a in (
        ["signals", ""], ["signals", "live"], ["signals", "live/start"], ["live", ""], ["system", ""], ["updates", ""],
        ["updates", "session"], ["diag", "logs"], ["casters", "onocoy"], ["receiver", "settings"], ["settings", ""], ["", ""])]
        + [("tab", n) for n in ("diag", "settings", "receiver", "casters", "")])
    assert out[0] == ["Status", "Casters", "Receiver", "Settings"]
    assert out[1:8] == ["receiver", "receiver/live", "receiver/live/start", "receiver/live", "settings/wifi", "settings/updates",
                        "settings/session"]
    # Pages that exist stay where they are; Diagnostics keeps its sub-routes.
    assert out[8:13] == [None] * 5
    # Diagnostics is reached from Settings: that tab is the one marked there.
    assert out[13:] == ["settings", "settings", "receiver", "casters", ""]


def test_every_link_in_the_ui_goes_to_a_page_that_exists():
    """No button, link or notice points at an address of the seven tabs (it would only redirect)."""
    pages = {"": None, "casters": None, "receiver": {"settings", "live"}, "settings": {"updates", "session", "wifi", "restart"},
             "diag": None}
    bad = []
    for f in sorted(UI_JS.glob("*.js")):
        for m in re.finditer(r"'#/([a-z]*)(?:/([a-z]+))?[a-z/]*'", f.read_text()):
            name, sub = m.group(1), m.group(2)
            if name not in pages or (sub and pages[name] is not None and sub not in pages[name]):
                bad.append(f"{f.name}: {m.group(0)}")
    assert bad == []


# Every notice at once: a slot stopped by the login guard, settings storage gone, maintenance, new Wi-Fi settings
# waiting, the first configuration held, a comparison running and an update.
BUSY = status(device={"settings_available": False},
              receiver={"rx_maintenance": True, "maintenance_left_s": 600, "first_apply_held": True,
                        "signal_test": {"state": "measuring", "phase": 0, "left_s": 300, "group": {"master": 3, "slave": 6}}},
              network={"online": True, "ssid": "Home", "cred_pending": True, "pending_ssid": "Cabin"},
              casters=[caster(0, "Onocoy", "stopped", stop_reason="duplicate")],
              update={"app_available": True, "app_version": "v0.4.0"})


def test_notices_one_line_one_button_worst_first():
    (lines,) = run_ui([("noticeLines", [BUSY, "diag"])])
    assert [ln[0] for ln in lines] == ["settings", "onocoy_stopped:0", "wifi", "maintenance", "held", "signal_test", "update"]
    assert [ln[1] for ln in lines] == ["bad", "bad", "warn", "info", "info", "info", "info"]
    # What needs the user now never folds away: settings storage, the stopped slot, maintenance.
    assert [ln[0] for ln in lines if ln[2]] == ["settings", "onocoy_stopped:0", "maintenance"]
    for key, _, _, line, buttons in lines:
        # One line each: the title and a few words; one button (the update adds Later).
        assert len(line) <= 70, line
        assert buttons == (2 if key == "update" else 1), key
    assert lines[0][3] == "Settings cannot be saved. Restart the station."
    assert lines[2][3] == "New Wi-Fi settings for Cabin not confirmed yet"
    assert lines[3][3] == "Receiver maintenance. Ends in about 10\u00a0min."


def test_notices_fold_after_two_but_never_the_urgent_ones():
    folded, opened = run_ui([("fold", [BUSY, "diag", False]), ("fold", [BUSY, "diag", True])])
    # The first two of the others (by tone), then "+2 more".
    assert folded == [["settings", "onocoy_stopped:0", "wifi", "maintenance", "held"], 2]
    assert opened == [["settings", "onocoy_stopped:0", "wifi", "maintenance", "held", "signal_test", "update"], 2]
    # Two or fewer others: nothing folds.
    calm = status(update={"app_available": True, "app_version": "v0.4.0"}, receiver={"first_apply_held": True})
    (f,) = run_ui([("fold", [calm, "casters", False])])
    assert f == [["held", "update"], 0]


def test_a_notice_hides_on_the_page_whose_card_says_it():
    safe = status(safe_mode={"active": True, "crashes": 3, "retry_in_s": 2690}, receiver={"state": "idle"},
                  casters=[caster(0, "Onocoy", "disabled")])
    uart = status(receiver={"state": "no_receiver", "uart_closed": True})
    journal = status(receiver={"journal": "waiting"})
    trial = status(network={"cred_trial": True, "cred_trial_left_s": 80, "pending_ssid": "Cabin"})
    pages = ["casters", "receiver", "receiver/settings", "settings", "diag"]
    cases = [(BUSY, p) for p in pages] + [(x, p) for x in (safe, uart, journal, trial) for p in ("", "settings", "receiver")]
    out = [[k for k, _ in o] for o in run_ui([("notices", list(c)) for c in cases])]
    every = {"settings", "onocoy_stopped:0", "wifi", "maintenance", "held", "signal_test", "update"}
    hidden = {"casters": {"onocoy_stopped:0"}, "receiver": {"held"}, "receiver/settings": {"maintenance", "signal_test"},
              "settings": {"wifi", "update"}, "diag": set()}
    for p, keys in zip(pages, out[:5]):
        assert set(keys) == every - hidden[p], p
    safe_k, uart_k, journal_k, trial_k = out[5:8], out[8:11], out[11:14], out[14:17]
    # The Status page's top card says each of them; Settings has safe mode, the update session and the Wi-Fi trial.
    assert safe_k == [[], [], ["safe_mode"]]
    assert uart_k == [[], ["uart_closed"], ["uart_closed"]]
    assert journal_k == [[], [], ["journal"]]
    assert trial_k == [[], [], ["wifi"]]

